/*
 * Local Register Recorder — Implementation
 *
 * Mounts the SPIFFS "storage" partition and writes periodic register
 * snapshots in JSONL format.  Each tick (≈500 ms) produces two lines:
 *
 *   {"t":<ms>,"fc":3,"addr":2000,"count":58,"values":[...]}
 *   {"t":<ms>,"fc":3,"addr":2100,"count":39,"values":[...]}
 *
 * The file is directly loadable by the playback engine.
 *
 * Approximate data rate: ~1 KB/s → 3.6 MB/h.
 *   4 MB flash (Atom S3)  → 2 MB storage  → ~33 min
 *   8 MB flash (Atom S3R) → 6 MB storage  → ~100 min
 */
#include "recorder.h"
#include "register_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static const char* TAG = "recorder";
static const char* MOUNT_POINT = "/spiffs";
static const char* REC_FILE    = "/spiffs/rec.jsonl";

namespace recorder {

// ============================================================================
// State
// ============================================================================

static bool     s_mounted   = false;
static bool     s_recording = false;
static FILE*    s_file      = nullptr;
static uint32_t s_entries   = 0;
static int64_t  s_start_us  = 0;       // esp_timer_get_time at start
static size_t   s_bytes     = 0;       // bytes written this session
static size_t   s_total_cap = 0;       // total partition size

// ============================================================================
// Helpers
// ============================================================================

// Write one register-range line to the file.
// Returns number of bytes written, or 0 on error.
static size_t writeSnapshot(FILE* f, uint32_t t_ms,
                            uint16_t addr, uint16_t count,
                            const uint16_t* data) {
    // Pre-format into a stack buffer.  Max line ≈ 50 + 58*6 = ~400 bytes.
    char buf[512];
    int pos = snprintf(buf, sizeof(buf),
                       "{\"t\":%lu,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                       (unsigned long)t_ms, addr, count);

    for (uint16_t i = 0; i < count && pos < (int)sizeof(buf) - 10; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%u", data[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}\n");

    size_t written = fwrite(buf, 1, (size_t)pos, f);
    return written;
}

static uint32_t elapsedMs() {
    if (s_start_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t init() {
    if (s_mounted) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path              = MOUNT_POINT;
    conf.partition_label        = "storage";
    conf.max_files              = 3;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    s_total_cap = total;
    s_mounted = true;

    ESP_LOGI(TAG, "SPIFFS mounted — %lu KB total, %lu KB used",
             (unsigned long)(total / 1024), (unsigned long)(used / 1024));
    return ESP_OK;
}

esp_err_t start() {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (s_recording) return ESP_OK;           // already running

    s_file = fopen(REC_FILE, "w");            // overwrite previous
    if (!s_file) {
        ESP_LOGE(TAG, "Failed to open %s for writing", REC_FILE);
        return ESP_FAIL;
    }

    s_entries  = 0;
    s_bytes    = 0;
    s_start_us = esp_timer_get_time();
    s_recording = true;
    ESP_LOGI(TAG, "Recording started → %s", REC_FILE);
    return ESP_OK;
}

void stop() {
    if (!s_recording) return;
    s_recording = false;
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = nullptr;
    }
    ESP_LOGI(TAG, "Recording stopped — %lu entries, %lu bytes",
             (unsigned long)s_entries, (unsigned long)s_bytes);
}

bool isRecording() { return s_recording; }

void tick() {
    if (!s_recording || !s_file) return;

    uint32_t t = elapsedMs();

    // Check remaining space (stop if < 4 KB free to avoid corrupting FS)
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    if (total > 0 && (total - used) < 4096) {
        ESP_LOGW(TAG, "Storage full — stopping recording");
        stop();
        return;
    }

    // Snapshot holding registers (2000–2057)
    size_t w = writeSnapshot(s_file, t,
                             reg::HOLDING_BASE, reg::HOLDING_COUNT,
                             reg::holdingData());
    s_bytes += w;
    s_entries++;

    // Snapshot input registers (2100–2138)
    w = writeSnapshot(s_file, t,
                      reg::INPUT_BASE, reg::INPUT_COUNT,
                      reg::inputData());
    s_bytes += w;
    s_entries++;

    // Flush every ~10 seconds (20 snapshots at 500 ms)
    if (s_entries % 40 == 0) {
        fflush(s_file);
    }
}

Status getStatus() {
    Status st = {};
    st.recording   = s_recording;
    st.entries     = s_entries;
    st.elapsed_ms  = s_recording ? elapsedMs() : 0;
    st.bytes_used  = s_bytes;
    st.bytes_total = s_total_cap;

    // If not recording, show file size on disk
    if (!s_recording && s_mounted) {
        struct stat sb;
        if (stat(REC_FILE, &sb) == 0) {
            st.bytes_used = (size_t)sb.st_size;
        }
    }
    return st;
}

esp_err_t deleteData() {
    if (s_recording) stop();
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    if (remove(REC_FILE) == 0 || errno == ENOENT) {
        ESP_LOGI(TAG, "Recording data deleted");
        s_entries = 0;
        s_bytes   = 0;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to delete %s", REC_FILE);
    return ESP_FAIL;
}

const char* getFilePath() { return REC_FILE; }

}  // namespace recorder
