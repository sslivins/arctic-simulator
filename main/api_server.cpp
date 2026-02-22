/*
 * REST API Server — Implementation
 *
 * Endpoints:
 *   GET  /api/status                — Simulator status + Modbus stats
 *   GET  /api/registers             — All register values
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
#include "playback.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "api";
static httpd_handle_t s_server = nullptr;

// Embedded gzipped web dashboard
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

// ============================================================================
// Helpers
// ============================================================================

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
// GET /api/status
// ============================================================================

static esp_err_t handleGetStatus(httpd_req_t* req) {
    auto stats = mb_slave::getStats();
    auto pb_status = playback::getStatus();

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "firmware", "arctic-simulator");
    cJSON_AddStringToObject(json, "version", "0.1.0");
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
// GET / — Serve embedded web dashboard
// ============================================================================

static esp_err_t handleWebUI(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    size_t len = index_html_gz_end - index_html_gz_start;
    httpd_resp_send(req, (const char*)index_html_gz_start, len);
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
    config.max_uri_handlers = 18;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    // Register URI handlers
    const httpd_uri_t uris[] = {
        { "/api/status",          HTTP_GET,  handleGetStatus,     nullptr },
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
        { "/",                    HTTP_GET,  handleWebUI,        nullptr },
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
