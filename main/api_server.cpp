/*
 * REST API Server — Implementation
 *
 * Endpoints:
 *   GET  /api/status                — Simulator status + Modbus stats
 *   GET  /api/heatpump              — Abstracted heat pump state
 *   GET  /api/registers             — All register values (raw)
 *   GET  /api/registers?addr=XXXX   — Single register
 *   PUT  /api/registers?addr=XXXX   — Set single register { "value": N }
 *   POST /api/registers/bulk        — Set multiple registers { "registers": { "2100": 350, ... } }
 *   POST /api/preset                — Load preset { "name": "heating" }
 *   POST /api/errors/clear          — Clear all error flags
 *   POST /api/playback/load         — Upload JSONL capture file
 *   POST /api/playback/start        — Start playback
 *   POST /api/playback/stop         — Stop playback
 *   GET  /api/playback/status       — Playback status
 */
#include "api_server.h"
#include "register_map.h"
#include "modbus_slave.h"
#include "simulation.h"
#include "playback.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "api";
static httpd_handle_t s_server = nullptr;

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

static const char* modeToString(uint16_t mode) {
    switch (mode) {
    case reg::MODE_COOLING:       return "cooling";
    case reg::MODE_FLOOR_HEATING: return "floor_heating";
    case reg::MODE_FAN_COIL_HEAT: return "fan_coil_heating";
    case reg::MODE_HOT_WATER:     return "hot_water";
    case reg::MODE_AUTO:          return "auto";
    default:                      return "unknown";
    }
}

static const char* fanSpeedString(uint16_t sts2) {
    if (sts2 & reg::STS2_FAN_HIGH) return "high";
    if (sts2 & reg::STS2_FAN_MED)  return "medium";
    if (sts2 & reg::STS2_FAN_LOW)  return "low";
    return "off";
}

static void addActiveErrors(cJSON* arr, uint16_t ec1, uint16_t ec2, uint16_t ec3) {
    // Error Code 1 (register 2134) — brine/tank sensor errors
    const char* ec1_names[] = {
        "brine_inlet_sensor", "brine_outlet_sensor", "brine_flow_protection", "tank_sensor"
    };
    for (int i = 0; i < 4; i++) {
        if (ec1 & (1 << i)) cJSON_AddItemToArray(arr, cJSON_CreateString(ec1_names[i]));
    }

    // Error Code 2 (register 2137) — sensor/communication errors
    const char* ec2_names[] = {
        "indoor_ee", "outdoor_ee", "inlet_water_sensor", "outlet_water_sensor",
        "antifreeze_protection", "external_coil_sensor", "discharge_sensor",
        "suction_sensor", "ambient_sensor", "drive_board_comm",
        "wired_controller_comm", "compressor_abnormal", "indoor_outdoor_comm",
        "ipm_error", "high_outlet_temp", "high_pressure"
    };
    for (int i = 0; i < 16; i++) {
        if (ec2 & (1 << i)) cJSON_AddItemToArray(arr, cJSON_CreateString(ec2_names[i]));
    }

    // Error Code 3 (register 2138) — protection errors
    const char* ec3_names[] = {
        "low_pressure", "discharge_overtemp", "outdoor_ambient_sensor",
        "suction_overtemp", "compressor_overcurrent", "dc_bus_overvoltage",
        "phase_loss", "ipm_overtemp", "fan_motor_error", "compressor_phase_error",
        "eev_sensor", "outdoor_comm", "water_flow_protection",
        "compressor_freq_limit", "dc_bus_undervoltage", "ac_overcurrent"
    };
    for (int i = 0; i < 16; i++) {
        if (ec3 & (1 << i)) cJSON_AddItemToArray(arr, cJSON_CreateString(ec3_names[i]));
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
static char* readBody(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 8192) return nullptr;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    int received = httpd_req_recv(req, buf, len);
    if (received != len) { free(buf); return nullptr; }
    buf[len] = '\0';
    return buf;
}

// ============================================================================
// GET /api/heatpump — abstracted heat pump state
// ============================================================================

static esp_err_t handleGetHeatpump(httpd_req_t* req) {
    uint16_t sts2 = reg::get(reg::STATUS_2);
    uint16_t sts3 = reg::get(reg::STATUS_3);

    cJSON* json = cJSON_CreateObject();

    // --- Power & operating mode ---
    cJSON_AddBoolToObject(json, "unit_on", (sts2 & reg::STS2_UNIT_ON) != 0);
    cJSON_AddStringToObject(json, "mode", modeToString(reg::get(reg::WORKING_MODE)));
    cJSON_AddNumberToObject(json, "mode_raw", reg::get(reg::WORKING_MODE));

    // --- Setpoints (commanded by controller) ---
    cJSON* sp = cJSON_AddObjectToObject(json, "setpoints");
    cJSON_AddNumberToObject(sp, "cooling", reg::get(reg::COOLING_SETPOINT));
    cJSON_AddNumberToObject(sp, "heating", reg::get(reg::HEATING_SETPOINT));
    cJSON_AddNumberToObject(sp, "hot_water", reg::get(reg::HOT_WATER_SETPOINT));

    // --- Temperatures ---
    cJSON* temps = cJSON_AddObjectToObject(json, "temperatures");
    cJSON_AddNumberToObject(temps, "outlet_water", reg::get(reg::OUTLET_WATER_TEMP));
    cJSON_AddNumberToObject(temps, "inlet_water", reg::get(reg::INLET_WATER_TEMP));
    cJSON_AddNumberToObject(temps, "water_tank", reg::get(reg::WATER_TANK_TEMP));
    cJSON_AddNumberToObject(temps, "outdoor_ambient", reg::get(reg::OUTDOOR_AMBIENT_TEMP));
    cJSON_AddNumberToObject(temps, "discharge", reg::get(reg::DISCHARGE_TEMP));
    cJSON_AddNumberToObject(temps, "suction", reg::get(reg::SUCTION_TEMP));
    cJSON_AddNumberToObject(temps, "outdoor_coil", reg::get(reg::OUTDOOR_COIL_TEMP));
    cJSON_AddNumberToObject(temps, "indoor_coil", reg::get(reg::INDOOR_COIL_TEMP));
    cJSON_AddNumberToObject(temps, "ipm", reg::get(reg::IPM_TEMP));

    // --- Compressor & electrical ---
    cJSON* comp = cJSON_AddObjectToObject(json, "compressor");
    cJSON_AddBoolToObject(comp, "running", (sts2 & reg::STS2_COMPRESSOR) != 0);
    cJSON_AddNumberToObject(comp, "frequency", reg::get(reg::COMPRESSOR_FREQ));
    cJSON_AddNumberToObject(comp, "phase_current", reg::get(reg::COMP_PHASE_CURRENT));

    cJSON* elec = cJSON_AddObjectToObject(json, "electrical");
    cJSON_AddNumberToObject(elec, "ac_voltage", reg::get(reg::AC_VOLTAGE));
    cJSON_AddNumberToObject(elec, "ac_current", reg::get(reg::AC_CURRENT));
    // DC voltage stored as raw × 10 — convert to actual volts
    cJSON_AddNumberToObject(elec, "dc_voltage", reg::get(reg::DC_VOLTAGE) / 10.0);

    // --- Pressure (stored as raw × 100 — convert to MPa) ---
    cJSON* pressure = cJSON_AddObjectToObject(json, "pressure");
    cJSON_AddNumberToObject(pressure, "high", reg::get(reg::HIGH_PRESSURE) / 100.0);
    cJSON_AddNumberToObject(pressure, "low", reg::get(reg::LOW_PRESSURE) / 100.0);

    // --- Peripherals ---
    cJSON* periph = cJSON_AddObjectToObject(json, "peripherals");
    cJSON_AddStringToObject(periph, "fan_speed", fanSpeedString(sts2));
    cJSON_AddNumberToObject(periph, "fan_rpm", reg::get(reg::FAN_SPEED));
    cJSON_AddBoolToObject(periph, "water_pump", (sts2 & reg::STS2_WATER_PUMP) != 0);
    cJSON_AddBoolToObject(periph, "water_flow", (sts2 & reg::STS2_WATER_FLOW) != 0);
    cJSON_AddBoolToObject(periph, "four_way_valve", (sts2 & reg::STS2_4WAY_VALVE) != 0);
    cJSON_AddBoolToObject(periph, "electric_heater", (sts2 & reg::STS2_ELEC_HEATER) != 0);
    cJSON_AddBoolToObject(periph, "three_way_v1", (sts2 & reg::STS2_3WAY_V1) != 0);
    cJSON_AddBoolToObject(periph, "three_way_v2", (sts2 & reg::STS2_3WAY_V2) != 0);
    cJSON_AddNumberToObject(periph, "primary_eev", reg::get(reg::PRIMARY_EEV));
    cJSON_AddNumberToObject(periph, "secondary_eev", reg::get(reg::SECONDARY_EEV));
    cJSON_AddBoolToObject(periph, "defrost", (sts3 & (1 << 5)) != 0);

    // --- Errors ---
    uint16_t ec1 = reg::get(reg::ERROR_CODE_1);
    uint16_t ec2 = reg::get(reg::ERROR_CODE_2);
    uint16_t ec3 = reg::get(reg::ERROR_CODE_3);
    bool has_errors = (ec1 | ec2 | ec3) != 0;
    cJSON_AddBoolToObject(json, "has_errors", has_errors);
    cJSON* errors = cJSON_AddArrayToObject(json, "errors");
    if (has_errors) {
        addActiveErrors(errors, ec1, ec2, ec3);
    }

    return sendJson(req, json);
}

// ============================================================================
// GET /api/status
// ============================================================================

static esp_err_t handleGetStatus(httpd_req_t* req) {
    auto stats = mb_slave::getStats();
    auto pb_status = playback::getStatus();

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "firmware", "arctic-simulator");
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(json, "version", app->version);
    cJSON_AddBoolToObject(json, "modbus_active", mb_slave::isInitialized());

    cJSON* mb = cJSON_AddObjectToObject(json, "modbus_stats");
    cJSON_AddNumberToObject(mb, "reads", stats.read_count);
    cJSON_AddNumberToObject(mb, "writes", stats.write_count);
    cJSON_AddNumberToObject(mb, "errors", stats.error_count);

    cJSON* pb = cJSON_AddObjectToObject(json, "playback");
    cJSON_AddStringToObject(pb, "state",
        pb_status.state == playback::State::IDLE    ? "idle" :
        pb_status.state == playback::State::LOADED  ? "loaded" :
        pb_status.state == playback::State::PLAYING ? "playing" :
        pb_status.state == playback::State::PAUSED  ? "paused" : "unknown");
    cJSON_AddNumberToObject(pb, "entries", pb_status.total_entries);
    cJSON_AddNumberToObject(pb, "position", pb_status.current_entry);

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
    else if (strcmp(n, "error_e01") == 0) preset = reg::Preset::ERROR_E01;
    else if (strcmp(n, "error_p01") == 0) preset = reg::Preset::ERROR_P01;
    else {
        cJSON_Delete(json);
        return sendError(req, 400, "Unknown preset. Valid: idle, heating, cooling, hot_water, defrost, error_e01, error_p01");
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
// Playback endpoints
// ============================================================================

static esp_err_t handlePlaybackLoad(httpd_req_t* req) {
    char* body = readBody(req);
    if (!body) return sendError(req, 400, "Request too large or empty");

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
    config.max_uri_handlers = 16;
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
        { "/api/playback/load",   HTTP_POST, handlePlaybackLoad,  nullptr },
        { "/api/playback/start",  HTTP_POST, handlePlaybackStart, nullptr },
        { "/api/playback/stop",   HTTP_POST, handlePlaybackStop,  nullptr },
        { "/api/playback/status", HTTP_GET,  handlePlaybackStatus,nullptr },
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
