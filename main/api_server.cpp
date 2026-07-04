/*
 * REST API Server — Implementation
 *
 * Endpoints:
 *   GET  /api/status                -- Simulator status + Tuya stats
 *   GET  /api/heatpump              — Abstracted heat pump state
 *   GET  /api/registers             — All register values (raw)
 *   GET  /api/registers?addr=XXXX   — Single register
 *   PUT  /api/registers?addr=XXXX   — Set single register { "value": N }
 *   POST /api/registers/bulk        — Set multiple registers { "registers": { "2100": 350, ... } }
 *   POST /api/preset                — Load preset { "name": "heating" }
 *   POST /api/errors/clear          — Clear all error flags
 *   POST /api/capture/arm           — Start recording raw RX bytes (reverse-eng)
 *   GET  /api/capture               — Stop + return captured raw RX bytes as hex
 *   GET  /api/commands              — Recent decoded fc=0x06 controller commands
 *   POST /api/playback/load         — Upload JSONL capture file
 *   POST /api/playback/start        — Start playback
 *   POST /api/playback/stop         — Stop playback
 *   GET  /api/playback/status       — Playback status
 *   POST /api/simulation             — Enable/disable simulation { "enabled": bool }
 *   GET  /api/stream                — SSE stream (real-time register snapshots)
 *   POST /api/stream/stop           — Stop active SSE stream
 *   POST /api/reboot                — Reboot the device
 */
#include "api_server.h"
#include "register_map.h"
#include "tuya_slave.h"
#include "simulation.h"
#include "playback.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "api";
static httpd_handle_t s_server = nullptr;

// SSE stream state
static volatile bool   s_streaming        = false;
static int             s_stream_fd         = -1;
static httpd_req_t*    s_stream_async_req  = nullptr;
static TaskHandle_t    s_stream_task       = nullptr;

// Embedded web dashboard — gzip compressed (main/web/index.html.gz via EMBED_FILES)
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

// ============================================================================
// Dashboard
// ============================================================================

static esp_err_t handleDashboard(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t len = index_html_gz_end - index_html_gz_start;
    httpd_resp_send(req, (const char*)index_html_gz_start, len);
    return ESP_OK;
}

// ============================================================================
// Helpers
// ============================================================================

static void addActiveFaults(cJSON* arr, uint16_t fault) {
    // Macon fault byte (reg 2128). Only bit7 (P01 water-flow) is confirmed;
    // any other set bit is reported generically so the raw value stays visible
    // for the ongoing fault-bit reverse-engineering.
    if (fault & reg::FAULT_P01_WATER_FLOW) {
        cJSON_AddItemToArray(arr, cJSON_CreateString("P01_water_flow"));
    }
    uint16_t undecoded = fault & ~(uint16_t)reg::FAULT_P01_WATER_FLOW;
    if (undecoded) {
        char buf[24];
        snprintf(buf, sizeof(buf), "undecoded_0x%02X", undecoded & 0xFF);
        cJSON_AddItemToArray(arr, cJSON_CreateString(buf));
    }
}

static esp_err_t sendJson(httpd_req_t* req, cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t sendError(httpd_req_t* req, int status, const char* msg) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", msg);
    char* str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                               status == 404 ? "404 Not Found" :
                               "500 Internal Server Error");
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

// Read request body into buffer (caller frees)
static char* readBody(httpd_req_t* req, int max_len = 8192) {
    int len = req->content_len;
    if (len <= 0 || len > max_len) return nullptr;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    int total = 0;
    while (total < len) {
        int received = httpd_req_recv(req, buf + total, len - total);
        if (received <= 0) { free(buf); return nullptr; }
        total += received;
    }
    buf[len] = '\0';
    return buf;
}

// ============================================================================
// GET /api/heatpump — abstracted heat pump state
// ============================================================================

static esp_err_t handleGetHeatpump(httpd_req_t* req) {
    uint16_t status = reg::get(reg::STATUS_BYTE);
    uint16_t fault  = reg::get(reg::FAULT);

    cJSON* json = cJSON_CreateObject();

    // --- Running state (derived from the Macon status byte, reg 2130) ---
    bool compressor_on = (status & reg::STS_COMPRESSOR) != 0;
    bool pump_on       = (status & reg::STS_WATER_PUMP) != 0;
    cJSON_AddBoolToObject(json, "running", compressor_on || pump_on);
    cJSON_AddNumberToObject(json, "status_raw", status);

    // --- Setpoint ---
    cJSON* sp = cJSON_AddObjectToObject(json, "setpoints");
    cJSON_AddNumberToObject(sp, "hot_water", (int8_t)reg::get(reg::HOT_WATER_SETPOINT));

    // --- Temperatures (whole °C, signed byte) ---
    cJSON* temps = cJSON_AddObjectToObject(json, "temperatures");
    cJSON_AddNumberToObject(temps, "outlet_water", (int8_t)reg::get(reg::OUTLET_WATER_TEMP));
    cJSON_AddNumberToObject(temps, "inlet_water", (int8_t)reg::get(reg::INLET_WATER_TEMP));
    cJSON_AddNumberToObject(temps, "water_tank", (int8_t)reg::get(reg::WATER_TANK_TEMP));
    cJSON_AddNumberToObject(temps, "outdoor_ambient", (int8_t)reg::get(reg::OUTDOOR_AMBIENT_TEMP));
    cJSON_AddNumberToObject(temps, "discharge", (int8_t)reg::get(reg::DISCHARGE_TEMP));
    cJSON_AddNumberToObject(temps, "suction", (int8_t)reg::get(reg::SUCTION_TEMP));
    cJSON_AddNumberToObject(temps, "coil", (int8_t)reg::get(reg::COIL_TEMP));
    cJSON_AddNumberToObject(temps, "cool_coil", (int8_t)reg::get(reg::COOL_COIL_TEMP));
    cJSON_AddNumberToObject(temps, "ipm", (int8_t)reg::get(reg::IPM_TEMP));

    // --- Compressor ---
    cJSON* comp = cJSON_AddObjectToObject(json, "compressor");
    cJSON_AddBoolToObject(comp, "running", compressor_on);
    cJSON_AddNumberToObject(comp, "frequency", reg::get(reg::COMPRESSOR_FREQ));

    // --- Electrical (display scales: voltages x10 V, power x100 W) ---
    cJSON* elec = cJSON_AddObjectToObject(json, "electrical");
    cJSON_AddNumberToObject(elec, "ac_voltage", reg::get(reg::AC_VOLTAGE) * 10);
    cJSON_AddNumberToObject(elec, "ac_current", reg::get(reg::AC_CURRENT));
    cJSON_AddNumberToObject(elec, "dc_bus_voltage", reg::get(reg::DC_BUS_VOLTAGE) * 10);
    cJSON_AddNumberToObject(elec, "realtime_power", reg::get(reg::REALTIME_POWER) * 100);

    // --- Peripherals ---
    cJSON* periph = cJSON_AddObjectToObject(json, "peripherals");
    cJSON_AddBoolToObject(periph, "water_pump", pump_on);
    cJSON_AddNumberToObject(periph, "main_eev", reg::get(reg::MAIN_EEV));
    cJSON_AddNumberToObject(periph, "fan_speed", reg::get(reg::DC_MOTOR_SPEED));

    // --- Faults (reg 2128) ---
    cJSON_AddBoolToObject(json, "has_faults", fault != 0);
    cJSON_AddNumberToObject(json, "fault_raw", fault);
    cJSON* faults = cJSON_AddArrayToObject(json, "faults");
    if (fault) {
        addActiveFaults(faults, fault);
    }

    return sendJson(req, json);
}

// ============================================================================
// GET /api/status
// ============================================================================

static esp_err_t handleGetStatus(httpd_req_t* req) {
    auto stats = tuya_slave::getStats();
    auto pb_status = playback::getStatus();

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "firmware", "arctic-simulator");
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(json, "version", app->version);
    cJSON_AddStringToObject(json, "hostname", wifi::getHostname());
    cJSON_AddBoolToObject(json, "tuya_active", tuya_slave::isInitialized());

    cJSON* tu = cJSON_AddObjectToObject(json, "tuya_stats");
    cJSON_AddNumberToObject(tu, "frames_seen", stats.frames_seen);
    cJSON_AddNumberToObject(tu, "requests_handled", stats.requests_handled);
    cJSON_AddNumberToObject(tu, "responses_sent", stats.responses_sent);
    cJSON_AddNumberToObject(tu, "parse_errors", stats.parse_errors);
    cJSON_AddNumberToObject(tu, "unknown_windows", stats.unknown_windows);
    cJSON_AddNumberToObject(tu, "snapshot_failures", stats.snapshot_failures);
    cJSON_AddNumberToObject(tu, "tx_truncated", stats.tx_truncated);
    cJSON_AddNumberToObject(tu, "uart_errors", stats.uart_errors);
    cJSON_AddNumberToObject(tu, "commands_seen", stats.commands_seen);

    cJSON* pb = cJSON_AddObjectToObject(json, "playback");
    cJSON_AddStringToObject(pb, "state",
        pb_status.state == playback::State::IDLE    ? "idle" :
        pb_status.state == playback::State::LOADED  ? "loaded" :
        pb_status.state == playback::State::PLAYING ? "playing" :
        pb_status.state == playback::State::PAUSED  ? "paused" : "unknown");
    cJSON_AddNumberToObject(pb, "entries", pb_status.total_entries);
    cJSON_AddNumberToObject(pb, "position", pb_status.current_entry);

    cJSON_AddBoolToObject(json, "simulation", simulation::isEnabled());

    // SSE stream
    cJSON* st = cJSON_AddObjectToObject(json, "stream");
    cJSON_AddBoolToObject(st, "active", (bool)s_streaming);

    return sendJson(req, json);
}

// ============================================================================
// GET /api/registers
// ============================================================================

static esp_err_t handleGetRegisters(httpd_req_t* req) {
    // Check for ?addr= query parameter
    char query[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char addr_str[8] = {};
        if (httpd_query_key_value(query, "addr", addr_str, sizeof(addr_str)) == ESP_OK) {
            uint16_t addr = (uint16_t)atoi(addr_str);
            if (!reg::isValid(addr)) {
                return sendError(req, 404, "Invalid register address");
            }
            cJSON* json = cJSON_CreateObject();
            cJSON_AddNumberToObject(json, "addr", addr);
            cJSON_AddNumberToObject(json, "value", reg::get(addr));
            cJSON_AddStringToObject(json, "type", reg::isHolding(addr) ? "holding" : "input");
            return sendJson(req, json);
        }
    }

    // Return all registers
    cJSON* json = cJSON_CreateObject();

    cJSON* holding = cJSON_AddObjectToObject(json, "holding");
    for (uint16_t a = reg::HOLDING_BASE; a <= reg::HOLDING_END; a++) {
        char key[8];
        snprintf(key, sizeof(key), "%u", a);
        cJSON_AddNumberToObject(holding, key, reg::get(a));
    }

    cJSON* input = cJSON_AddObjectToObject(json, "input");
    for (uint16_t a = reg::INPUT_BASE; a <= reg::INPUT_END; a++) {
        char key[8];
        snprintf(key, sizeof(key), "%u", a);
        cJSON_AddNumberToObject(input, key, reg::get(a));
    }

    return sendJson(req, json);
}

// ============================================================================
// PUT /api/registers?addr=XXXX  — body: { "value": N }
// ============================================================================

static esp_err_t handlePutRegister(httpd_req_t* req) {
    char query[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return sendError(req, 400, "Missing ?addr= parameter");
    }
    char addr_str[8] = {};
    if (httpd_query_key_value(query, "addr", addr_str, sizeof(addr_str)) != ESP_OK) {
        return sendError(req, 400, "Missing ?addr= parameter");
    }
    uint16_t addr = (uint16_t)atoi(addr_str);
    if (!reg::isValid(addr)) {
        return sendError(req, 404, "Invalid register address");
    }

    char* body = readBody(req);
    if (!body) return sendError(req, 400, "Invalid body");

    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) return sendError(req, 400, "Invalid JSON");

    cJSON* val = cJSON_GetObjectItem(json, "value");
    if (!val || !cJSON_IsNumber(val)) {
        cJSON_Delete(json);
        return sendError(req, 400, "Missing 'value' field");
    }

    uint16_t value = (uint16_t)val->valueint;
    cJSON_Delete(json);

    reg::set(addr, value);
    if (reg::isHolding(addr)) simulation::updateStatus();
    ESP_LOGI(TAG, "Set register %u = %u", addr, value);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "addr", addr);
    cJSON_AddNumberToObject(resp, "value", value);
    return sendJson(req, resp);
}

// ============================================================================
// POST /api/registers/bulk  — body: { "registers": { "2100": 350, ... } }
// ============================================================================

static esp_err_t handleBulkSet(httpd_req_t* req) {
    char* body = readBody(req);
    if (!body) return sendError(req, 400, "Invalid body");

    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) return sendError(req, 400, "Invalid JSON");

    cJSON* regs = cJSON_GetObjectItem(json, "registers");
    if (!regs || !cJSON_IsObject(regs)) {
        cJSON_Delete(json);
        return sendError(req, 400, "Missing 'registers' object");
    }

    int count = 0;
    cJSON* item;
    cJSON_ArrayForEach(item, regs) {
        uint16_t addr = (uint16_t)atoi(item->string);
        if (reg::isValid(addr) && cJSON_IsNumber(item)) {
            reg::set(addr, (uint16_t)item->valueint);
            count++;
        }
    }
    cJSON_Delete(json);

    simulation::updateStatus();
    ESP_LOGI(TAG, "Bulk set %d registers", count);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "updated", count);
    return sendJson(req, resp);
}

// ============================================================================
// POST /api/simulation  — body: { "enabled": true/false }
// ============================================================================

static esp_err_t handleSimulation(httpd_req_t* req) {
    char* body = readBody(req);
    if (!body) return sendError(req, 400, "Invalid body");

    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) return sendError(req, 400, "Invalid JSON");

    cJSON* enabled = cJSON_GetObjectItem(json, "enabled");
    if (!enabled || !cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return sendError(req, 400, "Missing 'enabled' boolean");
    }

    simulation::setEnabled(cJSON_IsTrue(enabled));
    cJSON_Delete(json);

    // If re-enabling, recompute status from current holding registers
    if (simulation::isEnabled()) simulation::updateStatus();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "simulation", simulation::isEnabled());
    return sendJson(req, resp);
}

// ============================================================================
// POST /api/preset  — body: { "name": "heating" }
// ============================================================================

static esp_err_t handlePreset(httpd_req_t* req) {
    char* body = readBody(req);
    if (!body) return sendError(req, 400, "Invalid body");

    cJSON* json = cJSON_Parse(body);
    free(body);
    if (!json) return sendError(req, 400, "Invalid JSON");

    cJSON* name = cJSON_GetObjectItem(json, "name");
    if (!name || !cJSON_IsString(name)) {
        cJSON_Delete(json);
        return sendError(req, 400, "Missing 'name' field");
    }

    const char* n = name->valuestring;
    reg::Preset preset;
    if      (strcmp(n, "idle") == 0)      preset = reg::Preset::IDLE;
    else if (strcmp(n, "heating") == 0)   preset = reg::Preset::HEATING;
    else if (strcmp(n, "cooling") == 0)   preset = reg::Preset::COOLING;
    else if (strcmp(n, "hot_water") == 0) preset = reg::Preset::HOT_WATER;
    else if (strcmp(n, "defrost") == 0)   preset = reg::Preset::DEFROST;
    else if (strcmp(n, "fault_p01") == 0) preset = reg::Preset::FAULT_P01;
    else {
        cJSON_Delete(json);
        return sendError(req, 400, "Unknown preset. Valid: idle, heating, cooling, hot_water, defrost, fault_p01");
    }
    cJSON_Delete(json);

    reg::loadPreset(preset);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "preset", n);
    cJSON_AddStringToObject(resp, "status", "loaded");
    return sendJson(req, resp);
}

// ============================================================================
// POST /api/errors/clear
// ============================================================================

static esp_err_t handleClearErrors(httpd_req_t* req) {
    reg::clearErrors();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "cleared");
    return sendJson(req, resp);
}

// ============================================================================
// Raw RX capture endpoints (reverse-engineering aid)
//   POST /api/capture/arm   — clear buffer + start recording raw RX bytes
//   GET  /api/capture       — stop recording + return captured bytes as hex
// ============================================================================

static esp_err_t handleCaptureArm(httpd_req_t* req) {
    tuya_slave::captureArm();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "armed");
    cJSON_AddNumberToObject(resp, "buffer_size", (double)tuya_slave::CAPTURE_BUF_SZ);
    return sendJson(req, resp);
}

static esp_err_t handleCaptureGet(httpd_req_t* req) {
    static uint8_t buf[tuya_slave::CAPTURE_BUF_SZ];
    size_t n = tuya_slave::captureCopy(buf, sizeof(buf));

    // Hex-encode: 2 chars per byte.
    char* hex = (char*)malloc(n * 2 + 1);
    if (!hex) return sendError(req, 500, "Out of memory");
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        hex[i * 2]     = H[buf[i] >> 4];
        hex[i * 2 + 1] = H[buf[i] & 0x0F];
    }
    hex[n * 2] = '\0';

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "captured");
    cJSON_AddNumberToObject(resp, "bytes", (double)n);
    cJSON_AddBoolToObject(resp, "truncated", n >= tuya_slave::CAPTURE_BUF_SZ);
    cJSON_AddStringToObject(resp, "hex", hex);
    free(hex);
    return sendJson(req, resp);
}

// GET /api/commands — recent decoded fc=0x06 controller commands
static esp_err_t handleGetCommands(httpd_req_t* req) {
    tuya_slave::CommandRec recs[tuya_slave::COMMAND_RING_SZ];
    uint32_t total = 0;
    size_t n = tuya_slave::getRecentCommands(recs, tuya_slave::COMMAND_RING_SZ, &total);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "total", (double)total);
    cJSON* arr = cJSON_AddArrayToObject(resp, "commands");
    for (size_t i = 0; i < n; ++i) {
        cJSON* c = cJSON_CreateObject();
        char sel[8], val[8], raw[24];
        snprintf(sel, sizeof(sel), "0x%04X", recs[i].field_a);
        snprintf(val, sizeof(val), "0x%04X", recs[i].field_b);
        snprintf(raw, sizeof(raw), "55AAF006%04X%04X", recs[i].field_a, recs[i].field_b);
        cJSON_AddStringToObject(c, "selector", sel);
        cJSON_AddStringToObject(c, "value", val);
        cJSON_AddNumberToObject(c, "value_dec", recs[i].field_b);
        cJSON_AddStringToObject(c, "frame", raw);
        cJSON_AddItemToArray(arr, c);
    }
    return sendJson(req, resp);
}

// ============================================================================
// Playback endpoints
// ============================================================================

static esp_err_t handlePlaybackLoad(httpd_req_t* req) {
    char* body = readBody(req, 65536);  // captures can be large
    if (!body) return sendError(req, 400, "Request too large (>64KB) or empty");

    esp_err_t err = playback::loadFromString(body);
    free(body);

    if (err != ESP_OK) {
        return sendError(req, 400, "Failed to parse capture data");
    }

    auto status = playback::getStatus();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "loaded");
    cJSON_AddNumberToObject(resp, "entries", status.total_entries);
    return sendJson(req, resp);
}

static esp_err_t handlePlaybackStart(httpd_req_t* req) {
    esp_err_t err = playback::start();
    if (err != ESP_OK) {
        return sendError(req, 400, "No capture loaded or already playing");
    }
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "playing");
    return sendJson(req, resp);
}

static esp_err_t handlePlaybackStop(httpd_req_t* req) {
    playback::stop();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "stopped");
    return sendJson(req, resp);
}

static esp_err_t handlePlaybackStatus(httpd_req_t* req) {
    auto status = playback::getStatus();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state",
        status.state == playback::State::IDLE    ? "idle" :
        status.state == playback::State::LOADED  ? "loaded" :
        status.state == playback::State::PLAYING ? "playing" :
        status.state == playback::State::PAUSED  ? "paused" : "unknown");
    cJSON_AddNumberToObject(json, "total_entries", status.total_entries);
    cJSON_AddNumberToObject(json, "current_entry", status.current_entry);
    cJSON_AddNumberToObject(json, "elapsed_ms", status.elapsed_ms);
    return sendJson(req, json);
}

// ============================================================================
// SSE register stream
// ============================================================================

static void stopActiveStream() {
    if (!s_streaming) return;
    s_streaming = false;
    // Wait for task to detect the flag and exit
    for (int i = 0; i < 20 && s_stream_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_stream_task) {
        ESP_LOGW(TAG, "Force-killing stream task");
        vTaskDelete(s_stream_task);
        s_stream_task = nullptr;
    }
    if (s_stream_async_req) {
        httpd_req_async_handler_complete(s_stream_async_req);
        s_stream_async_req = nullptr;
    }
    s_stream_fd = -1;
}

static void streamTask(void* /*arg*/) {
    const int fd = s_stream_fd;
    httpd_handle_t hd = s_server;

    // Send HTTP response headers directly on the socket
    const char* hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";

    if (httpd_socket_send(hd, fd, hdr, strlen(hdr), 0) < 0) {
        ESP_LOGE(TAG, "Stream: failed to send headers");
        goto cleanup;
    }

    {
        int64_t start_us = esp_timer_get_time();

        while (s_streaming) {
            uint32_t t_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
            char line[512];
            int pos;

            // Holding registers
            pos = snprintf(line, sizeof(line),
                "data: {\"t\":%lu,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                (unsigned long)t_ms, (unsigned)reg::HOLDING_BASE,
                (unsigned)reg::HOLDING_COUNT);
            const uint16_t* hdata = reg::holdingData();
            for (int i = 0; i < reg::HOLDING_COUNT && pos < (int)sizeof(line) - 10; i++) {
                if (i > 0) line[pos++] = ',';
                pos += snprintf(line + pos, sizeof(line) - pos, "%u", hdata[i]);
            }
            pos += snprintf(line + pos, sizeof(line) - pos, "]}\n\n");
            if (httpd_socket_send(hd, fd, line, (size_t)pos, 0) < 0) break;

            // Input registers
            pos = snprintf(line, sizeof(line),
                "data: {\"t\":%lu,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                (unsigned long)t_ms, (unsigned)reg::INPUT_BASE,
                (unsigned)reg::INPUT_COUNT);
            const uint16_t* idata = reg::inputData();
            for (int i = 0; i < reg::INPUT_COUNT && pos < (int)sizeof(line) - 10; i++) {
                if (i > 0) line[pos++] = ',';
                pos += snprintf(line + pos, sizeof(line) - pos, "%u", idata[i]);
            }
            pos += snprintf(line + pos, sizeof(line) - pos, "]}\n\n");
            if (httpd_socket_send(hd, fd, line, (size_t)pos, 0) < 0) break;

            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

cleanup:
    ESP_LOGI(TAG, "Stream ended (fd %d)", s_stream_fd);
    if (s_stream_async_req) {
        httpd_req_async_handler_complete(s_stream_async_req);
        s_stream_async_req = nullptr;
    }
    s_stream_fd = -1;
    s_streaming = false;
    s_stream_task = nullptr;
    vTaskDelete(nullptr);
}

static esp_err_t handleStream(httpd_req_t* req) {
    // Stop any existing stream first
    stopActiveStream();

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return sendError(req, 500, "Failed to get socket");
    }

    httpd_req_t* async_req = nullptr;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Async handler begin failed: %s", esp_err_to_name(err));
        return sendError(req, 500, "Failed to start stream");
    }

    s_stream_fd        = fd;
    s_stream_async_req = async_req;
    s_streaming        = true;

    BaseType_t ret = xTaskCreate(streamTask, "sse_stream", 4096,
                                 nullptr, 5, &s_stream_task);
    if (ret != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        s_streaming        = false;
        s_stream_fd        = -1;
        s_stream_async_req = nullptr;
        return sendError(req, 500, "Task creation failed");
    }

    ESP_LOGI(TAG, "SSE stream started on fd %d", fd);
    return ESP_OK;
}

static esp_err_t handleStreamStop(httpd_req_t* req) {
    stopActiveStream();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "stopped");
    return sendJson(req, resp);
}

// ============================================================================
// Reboot
// ============================================================================

static void reboot_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handleReboot(httpd_req_t* req) {
    ESP_LOGW(TAG, "Reboot requested via API");
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "rebooting");
    sendJson(req, resp);

    // Delay reboot so the HTTP response can flush
    xTaskCreate(reboot_task, "reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

// ============================================================================
// CORS preflight
// ============================================================================

static esp_err_t handleOptions(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ============================================================================
// Server lifecycle
// ============================================================================

namespace api {

esp_err_t start() {
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    // Register URI handlers
    const httpd_uri_t uris[] = {
        { "/",                    HTTP_GET,  handleDashboard,     nullptr },
        { "/api/status",          HTTP_GET,  handleGetStatus,     nullptr },
        { "/api/heatpump",        HTTP_GET,  handleGetHeatpump,   nullptr },
        { "/api/registers",       HTTP_GET,  handleGetRegisters,  nullptr },
        { "/api/registers",       HTTP_PUT,  handlePutRegister,   nullptr },
        { "/api/registers/bulk",  HTTP_POST, handleBulkSet,       nullptr },
        { "/api/preset",          HTTP_POST, handlePreset,        nullptr },
        { "/api/errors/clear",    HTTP_POST, handleClearErrors,   nullptr },
        { "/api/capture/arm",     HTTP_POST, handleCaptureArm,    nullptr },
        { "/api/capture",         HTTP_GET,  handleCaptureGet,    nullptr },
        { "/api/commands",        HTTP_GET,  handleGetCommands,   nullptr },
        { "/api/simulation",     HTTP_POST, handleSimulation,    nullptr },
        { "/api/playback/load",   HTTP_POST, handlePlaybackLoad,  nullptr },
        { "/api/playback/start",  HTTP_POST, handlePlaybackStart, nullptr },
        { "/api/playback/stop",   HTTP_POST, handlePlaybackStop,  nullptr },
        { "/api/playback/status", HTTP_GET,  handlePlaybackStatus,nullptr },
        { "/api/stream",           HTTP_GET,  handleStream,          nullptr },
        { "/api/stream/stop",      HTTP_POST, handleStreamStop,      nullptr },
        { "/api/reboot",           HTTP_POST, handleReboot,          nullptr },
        { "/api/*",               HTTP_OPTIONS, handleOptions,    nullptr },
    };

    for (const auto& uri : uris) {
        httpd_register_uri_handler(s_server, &uri);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d (%d endpoints)",
             config.server_port, (int)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}

void stop() {
    stopActiveStream();
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

bool isRunning() {
    return s_server != nullptr;
}

}  // namespace api
