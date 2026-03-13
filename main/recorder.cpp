/*
 * Register Recorder — PSRAM Implementation
 *
 * Allocates a large buffer in PSRAM and writes periodic register
 * snapshots in JSONL format.  Each tick (≈500 ms) produces two lines:
 *
 *   {"t":<ms>,"fc":3,"addr":2000,"count":58,"values":[...]}
 *   {"t":<ms>,"fc":3,"addr":2100,"count":39,"values":[...]}
 *
 * The buffer is directly downloadable via the REST API.
 *
 * On devices without PSRAM the recorder reports unavailable and all
 * mutating calls are no-ops.
 *
 * Approximate data rate: ~1 KB/s → 2 MB buffer → ~34 min.
 */
#include "recorder.h"
#include "register_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "recorder";

// Buffer size to allocate in PSRAM (2 MB).
static const size_t BUFFER_SIZE = 2 * 1024 * 1024;

namespace recorder {

// ============================================================================
// State
// ============================================================================

static bool     s_available = false;   // PSRAM buffer allocated
static bool     s_recording = false;
static uint8_t* s_buf       = nullptr; // PSRAM buffer
static size_t   s_buf_cap   = 0;       // allocated size
static size_t   s_buf_pos   = 0;       // write cursor
static uint32_t s_entries   = 0;
static int64_t  s_start_us  = 0;       // esp_timer_get_time at start

// ============================================================================
// Helpers
// ============================================================================

// Append one register-range line to the buffer.
// Returns number of bytes written, or 0 if buffer full.
static size_t writeSnapshot(uint32_t t_ms,
                            uint16_t addr, uint16_t count,
                            const uint16_t* data) {
    // Format into a stack buffer.  Max line ≈ 50 + 58*6 = ~400 bytes.
    char line[512];
    int pos = snprintf(line, sizeof(line),
                       "{\"t\":%lu,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                       (unsigned long)t_ms, addr, count);

    for (uint16_t i = 0; i < count && pos < (int)sizeof(line) - 10; i++) {
        if (i > 0) line[pos++] = ',';
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%u", data[i]);
    }
    pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "]}\n");

    size_t len = (size_t)pos;
    if (s_buf_pos + len > s_buf_cap) {
        return 0;  // would overflow
    }
    memcpy(s_buf + s_buf_pos, line, len);
    s_buf_pos += len;
    return len;
}

static uint32_t elapsedMs() {
    if (s_start_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t init() {
    if (s_available) return ESP_OK;

    // Check for PSRAM
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram == 0) {
        ESP_LOGW(TAG, "No PSRAM detected — recorder unavailable");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "PSRAM detected: %lu KB total", (unsigned long)(psram / 1024));

    // Allocate recording buffer
    s_buf = (uint8_t*)heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu KB buffer in PSRAM",
                 (unsigned long)(BUFFER_SIZE / 1024));
        return ESP_ERR_NO_MEM;
    }

    s_buf_cap   = BUFFER_SIZE;
    s_buf_pos   = 0;
    s_available = true;

    ESP_LOGI(TAG, "Recording buffer: %lu KB in PSRAM",
             (unsigned long)(BUFFER_SIZE / 1024));
    return ESP_OK;
}

bool isAvailable() { return s_available; }

esp_err_t start() {
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;
    if (s_recording) return ESP_OK;

    // Reset buffer
    s_buf_pos   = 0;
    s_entries   = 0;
    s_start_us  = esp_timer_get_time();
    s_recording = true;
    ESP_LOGI(TAG, "Recording started (buffer: %lu KB)",
             (unsigned long)(s_buf_cap / 1024));
    return ESP_OK;
}

void stop() {
    if (!s_recording) return;
    s_recording = false;
    ESP_LOGI(TAG, "Recording stopped — %lu entries, %lu bytes",
             (unsigned long)s_entries, (unsigned long)s_buf_pos);
}

bool isRecording() { return s_recording; }

void tick() {
    if (!s_recording || !s_buf) return;

    uint32_t t = elapsedMs();

    // Snapshot holding registers (2000–2057)
    size_t w = writeSnapshot(t,
                             reg::HOLDING_BASE, reg::HOLDING_COUNT,
                             reg::holdingData());
    if (w == 0) {
        ESP_LOGW(TAG, "Buffer full — stopping recording");
        stop();
        return;
    }
    s_entries++;

    // Snapshot input registers (2100–2138)
    w = writeSnapshot(t,
                      reg::INPUT_BASE, reg::INPUT_COUNT,
                      reg::inputData());
    if (w == 0) {
        ESP_LOGW(TAG, "Buffer full — stopping recording");
        stop();
        return;
    }
    s_entries++;
}

Status getStatus() {
    Status st = {};
    st.available   = s_available;
    st.recording   = s_recording;
    st.entries     = s_entries;
    st.elapsed_ms  = s_recording ? elapsedMs() : 0;
    st.bytes_used  = s_buf_pos;
    st.bytes_total = s_buf_cap;
    return st;
}

void clear() {
    if (s_recording) stop();
    s_buf_pos = 0;
    s_entries = 0;
    ESP_LOGI(TAG, "Buffer cleared");
}

const uint8_t* bufferData() { return s_buf; }
size_t          bufferSize() { return s_buf_pos; }

}  // namespace recorder
