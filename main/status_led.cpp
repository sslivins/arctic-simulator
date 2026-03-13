/*
 * Status LED driver — Implementation
 *
 * Uses the ESP-IDF led_strip component to drive the single SK6812
 * addressable LED on the top of the M5Stack Atom S3 / S3R.
 *
 * GPIO 35 is the default (configurable via Kconfig).
 */
#include "status_led.h"

#include "led_strip.h"
#include "esp_log.h"

static const char* TAG = "status_led";

static led_strip_handle_t s_strip = nullptr;

// Keep brightness modest so it's visible but not blinding
static constexpr uint8_t BRIGHTNESS = 20;

esp_err_t status_led::init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num    = CONFIG_SIMULATOR_STATUS_LED_GPIO;
    strip_cfg.max_leds          = 1;
    strip_cfg.led_model         = LED_MODEL_SK6812;
    strip_cfg.led_pixel_format  = LED_PIXEL_FORMAT_GRB;
    strip_cfg.flags.invert_out  = false;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src        = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz  = 10 * 1000 * 1000;  // 10 MHz
    rmt_cfg.flags.with_dma = false;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start with LED off
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "Status LED ready (GPIO %d)", CONFIG_SIMULATOR_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led::setGreen() {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, 0, BRIGHTNESS, 0);
    led_strip_refresh(s_strip);
}

void status_led::setRed() {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, BRIGHTNESS, 0, 0);
    led_strip_refresh(s_strip);
}

void status_led::off() {
    if (!s_strip) return;
    led_strip_clear(s_strip);
}
