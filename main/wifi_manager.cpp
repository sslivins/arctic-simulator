/*
 * WiFi Manager — Implementation
 */
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char* TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

namespace wifi {

static EventGroupHandle_t s_event_group = nullptr;
static int s_retry_count = 0;
static bool s_connected = false;
static char s_ip_str[16] = "0.0.0.0";

static void eventHandler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Reconnecting... attempt %d/%d", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Connection failed after %d retries", MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected — IP: %s", s_ip_str);
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t init() {
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, CONFIG_SIMULATOR_WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, CONFIG_SIMULATOR_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", CONFIG_SIMULATOR_WIFI_SSID);

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        // Start mDNS
        esp_err_t err = mdns_init();
        if (err == ESP_OK) {
            mdns_hostname_set(CONFIG_SIMULATOR_MDNS_HOSTNAME);
            mdns_instance_name_set("Arctic Heat Pump Simulator");
            mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
            ESP_LOGI(TAG, "mDNS: %s.local", CONFIG_SIMULATOR_MDNS_HOSTNAME);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return ESP_FAIL;
}

bool isConnected() { return s_connected; }
const char* getIPAddress() { return s_ip_str; }

}  // namespace wifi
