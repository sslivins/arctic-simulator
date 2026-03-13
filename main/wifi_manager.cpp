/*
 * WiFi Manager — Implementation
 *
 * Flow:
 *   1. Load credentials from NVS namespace "wifi_creds".
 *      If NVS is empty, check Kconfig defaults (menuconfig).
 *   2. If we have an SSID → try STA mode (MAX_RETRY attempts).
 *   3. If STA fails or no SSID → start SoftAP + captive portal.
 *   4. Captive portal serves a scan-results page at 192.168.4.1.
 *      DNS redirects all queries to the portal (captive-portal UX).
 *   5. On form submit → save creds to NVS → reboot.
 */
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "dns_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "wifi";

// ── Event bits ─────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5
#define STA_TIMEOUT_MS     20000

// ── NVS keys ───────────────────────────────────────────────────────────
static const char* NVS_NAMESPACE = "wifi_creds";
static const char* NVS_KEY_SSID  = "ssid";
static const char* NVS_KEY_PASS  = "pass";

// ── Embedded captive-portal page ───────────────────────────────────────
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[]   asm("_binary_portal_html_gz_end");

namespace wifi {

// ============================================================================
// State
// ============================================================================

static EventGroupHandle_t s_event_group = nullptr;
static int  s_retry_count = 0;
static Mode s_mode        = Mode::IDLE;
static char s_ip_str[16]  = "0.0.0.0";
static char s_ap_name[33] = "ArcticSim";
static char s_hostname[64] = "";  // resolved mDNS hostname

static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif  = nullptr;
static httpd_handle_t s_portal_server = nullptr;

// Scan results cached for the portal API
static wifi_ap_record_t s_scan_results[20];
static uint16_t s_scan_count = 0;

// ============================================================================
// NVS helpers
// ============================================================================

static bool loadCredentials(char* ssid, size_t ssid_len,
                            char* pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = true;
    size_t len = ssid_len;
    if (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) != ESP_OK) ok = false;
    len = pass_len;
    if (ok && nvs_get_str(h, NVS_KEY_PASS, pass, &len) != ESP_OK) {
        // Password is optional for open networks
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok && ssid[0] != '\0';
}

static esp_err_t saveCredentials(const char* ssid, const char* pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ============================================================================
// STA event handler
// ============================================================================

static void staEventHandler(void* arg, esp_event_base_t base,
                            int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_mode = Mode::CONNECTING;
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Reconnecting… attempt %d/%d", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "STA connection failed after %d attempts", MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = (ip_event_got_ip_t*)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Connected — IP: %s", s_ip_str);
        s_retry_count = 0;
        s_mode = Mode::CONNECTED;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

// ============================================================================
// Start mDNS (called after STA connects)
// ============================================================================

static void startMDNS() {
    if (mdns_init() != ESP_OK) return;

    const char* base = CONFIG_SIMULATOR_MDNS_HOSTNAME;
    char candidate[64];
    esp_ip4_addr_t addr;

    // Try the base name first, then base-2, base-3, …
    for (int i = 1; i <= 9; i++) {
        if (i == 1)
            snprintf(candidate, sizeof(candidate), "%s", base);
        else
            snprintf(candidate, sizeof(candidate), "%s-%d", base, i);

        // Probe the network — short timeout (250 ms)
        esp_err_t err = mdns_query_a(candidate, 250, &addr);
        if (err == ESP_ERR_NOT_FOUND) {
            // Nobody answered → name is available
            break;
        }
        ESP_LOGI(TAG, "mDNS: %s.local already taken, trying next", candidate);
    }

    mdns_hostname_set(candidate);
    strncpy(s_hostname, candidate, sizeof(s_hostname) - 1);
    mdns_instance_name_set("Arctic Heat Pump Simulator");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", candidate);
}

// ============================================================================
// STA mode — attempt connection with stored credentials
// ============================================================================

static bool trySTA(const char* ssid, const char* pass) {
    ESP_LOGI(TAG, "Trying STA → '%s'", ssid);
    s_mode = Mode::CONNECTING;
    s_retry_count = 0;

    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pass && pass[0])
        strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK
                                                    : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(STA_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        startMDNS();
        return true;
    }

    // Stop STA so we can switch to AP
    esp_wifi_stop();
    return false;
}

// ============================================================================
// Captive-portal HTTP handlers
// ============================================================================

static esp_err_t handlePortalPage(httpd_req_t* req) {
    size_t len = (size_t)(portal_html_gz_end - portal_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char*)portal_html_gz_start, len);
    return ESP_OK;
}

// Android / iOS captive-portal detection endpoints — redirect to portal
static esp_err_t handleCaptiveRedirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// GET /api/scan — trigger or return cached scan results
static esp_err_t handleScan(httpd_req_t* req) {
    // Start a fresh scan
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_wifi_scan_start(&scan_cfg, true);  // blocking
    s_scan_count = 20;
    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_results);

    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", (char*)s_scan_results[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", s_scan_results[i].rssi);
        cJSON_AddNumberToObject(obj, "auth", s_scan_results[i].authmode);
        cJSON_AddItemToArray(arr, obj);
    }

    char* str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(arr);
    return ESP_OK;
}

// POST /api/connect — { "ssid": "...", "pass": "..." }
static esp_err_t handleConnect(httpd_req_t* req) {
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    cJSON* json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* jssid = cJSON_GetObjectItem(json, "ssid");
    cJSON* jpass = cJSON_GetObjectItem(json, "pass");
    if (!jssid || !cJSON_IsString(jssid) || strlen(jssid->valuestring) == 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }

    const char* ssid = jssid->valuestring;
    const char* pass = (jpass && cJSON_IsString(jpass)) ? jpass->valuestring : "";

    ESP_LOGI(TAG, "Portal: saving creds for '%s' and rebooting", ssid);
    saveCredentials(ssid, pass);
    cJSON_Delete(json);

    // Respond before rebooting
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"saved\",\"message\":\"Rebooting…\"}");

    // Give the response time to flush, then reboot
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;  // unreachable
}

// ============================================================================
// SoftAP provisioning mode
// ============================================================================

static void startProvisioning() {
    s_mode = Mode::PROVISIONING;

    // Build AP name with last 4 hex digits of MAC
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_name, sizeof(s_ap_name), "ArcticSim-%02X%02X",
                 mac[4], mac[5]);
    }

    ESP_LOGI(TAG, "Starting SoftAP: %s", s_ap_name);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, s_ap_name, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_ap_name);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;

    // Use APSTA so we can scan while serving the portal
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start DNS server that redirects everything to 192.168.4.1
    start_dns_server();

    // Start captive-portal HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 10;
    http_cfg.stack_size = 8192;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_portal_server, &http_cfg) == ESP_OK) {
        // Portal page
        httpd_uri_t portal = { "/", HTTP_GET, handlePortalPage, nullptr };
        httpd_register_uri_handler(s_portal_server, &portal);

        // WiFi scan API
        httpd_uri_t scan = { "/api/scan", HTTP_GET, handleScan, nullptr };
        httpd_register_uri_handler(s_portal_server, &scan);

        // Connect API
        httpd_uri_t conn = { "/api/connect", HTTP_POST, handleConnect, nullptr };
        httpd_register_uri_handler(s_portal_server, &conn);

        // Captive portal detection endpoints
        static const char* redir_paths[] = {
            "/generate_204",         // Android
            "/gen_204",              // Android
            "/hotspot-detect.html",  // Apple
            "/library/test/success.html",  // Apple
            "/connecttest.txt",      // Windows
            "/redirect",             // Windows
            "/ncsi.txt",             // Windows
        };
        for (auto* p : redir_paths) {
            httpd_uri_t r = { p, HTTP_GET, handleCaptiveRedirect, nullptr };
            httpd_register_uri_handler(s_portal_server, &r);
        }

        // Catch-all for any other path → redirect to portal
        httpd_uri_t catchall = { "/*", HTTP_GET, handleCaptiveRedirect, nullptr };
        httpd_register_uri_handler(s_portal_server, &catchall);

        ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1/");
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t init() {
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &staEventHandler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &staEventHandler, nullptr, nullptr));

    // Try loading credentials from NVS
    char ssid[33] = {};
    char pass[65] = {};
    bool have_creds = loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass));

    // Fall back to Kconfig defaults if NVS is empty
    if (!have_creds) {
        const char* kc_ssid = CONFIG_SIMULATOR_WIFI_SSID;
        if (kc_ssid[0] != '\0') {
            strncpy(ssid, kc_ssid, sizeof(ssid) - 1);
            strncpy(pass, CONFIG_SIMULATOR_WIFI_PASSWORD, sizeof(pass) - 1);
            have_creds = true;
            ESP_LOGI(TAG, "Using Kconfig WiFi credentials");
        }
    }

    if (have_creds) {
        if (trySTA(ssid, pass)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "STA failed — falling back to provisioning");
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting provisioning");
    }

    startProvisioning();
    return ESP_ERR_NOT_FINISHED;  // provisioning active
}

Mode getMode()              { return s_mode; }
bool isConnected()          { return s_mode == Mode::CONNECTED; }
const char* getIPAddress()  { return s_ip_str; }
const char* getAPName()     { return s_ap_name; }
const char* getHostname()   { return s_hostname; }

esp_err_t eraseCredentials() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials erased");
    return err;
}

}  // namespace wifi
