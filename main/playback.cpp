/*
 * Playback Engine — Implementation
 *
 * Stores capture entries in memory (limited by Atom S3's ~300KB free heap).
 * Each entry is a snapshot: a timestamp + register address range + values.
 * During playback, a timer ticks and applies entries as their timestamps pass.
 */
#include "playback.h"
#include "register_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <vector>

static const char* TAG = "playback";

namespace playback {

// ============================================================================
// Internal types
// ============================================================================

struct Entry {
    uint32_t timestamp_ms;  // Milliseconds since capture start
    uint16_t addr;          // Starting register address
    uint16_t count;         // Number of registers
    uint16_t* values;       // Heap-allocated array of values
};

// ============================================================================
// State
// ============================================================================

static State s_state = State::IDLE;
static std::vector<Entry> s_entries;
static uint32_t s_current = 0;
static int64_t  s_start_time = 0;  // microseconds (esp_timer_get_time)

// ============================================================================
// Helpers
// ============================================================================

static void freeEntries() {
    for (auto& e : s_entries) {
        free(e.values);
    }
    s_entries.clear();
    s_current = 0;
}

static uint32_t elapsedMs() {
    if (s_start_time == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_time) / 1000);
}

// ============================================================================
// Public API
// ============================================================================

void init() {
    s_state = State::IDLE;
    ESP_LOGI(TAG, "Playback engine initialized");
}

esp_err_t loadFromString(const char* jsonl_data) {
    // Stop any active playback
    if (s_state == State::PLAYING) stop();
    freeEntries();

    if (!jsonl_data || jsonl_data[0] == '\0') {
        ESP_LOGW(TAG, "Empty capture data");
        return ESP_ERR_INVALID_ARG;
    }

    // Parse line by line
    const char* line_start = jsonl_data;
    int line_num = 0;
    int parsed = 0;

    while (*line_start) {
        // Find end of line
        const char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        // Skip empty lines
        if (line_len == 0) {
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }

        line_num++;

        // Make a null-terminated copy of the line
        char* line = (char*)malloc(line_len + 1);
        if (!line) {
            ESP_LOGE(TAG, "Out of memory at line %d", line_num);
            break;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        // Strip trailing \r
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        // Parse JSON
        cJSON* json = cJSON_Parse(line);
        free(line);

        if (!json) {
            ESP_LOGW(TAG, "Skipping invalid JSON at line %d", line_num);
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }

        Entry entry = {};

        cJSON* t = cJSON_GetObjectItem(json, "t");
        if (t && cJSON_IsNumber(t)) entry.timestamp_ms = (uint32_t)t->valueint;

        cJSON* addr_item = cJSON_GetObjectItem(json, "addr");
        if (addr_item && cJSON_IsNumber(addr_item)) entry.addr = (uint16_t)addr_item->valueint;

        cJSON* fc = cJSON_GetObjectItem(json, "fc");
        int func_code = (fc && cJSON_IsNumber(fc)) ? fc->valueint : 3;

        if (func_code == 6) {
            // Single write: { "addr": XXXX, "value": N }
            cJSON* val = cJSON_GetObjectItem(json, "value");
            if (val && cJSON_IsNumber(val)) {
                entry.count = 1;
                entry.values = (uint16_t*)malloc(sizeof(uint16_t));
                if (entry.values) {
                    entry.values[0] = (uint16_t)val->valueint;
                    s_entries.push_back(entry);
                    parsed++;
                }
            }
        } else {
            // Multi-register: { "addr": XXXX, "count": N, "values": [...] }
            cJSON* count_item = cJSON_GetObjectItem(json, "count");
            cJSON* values = cJSON_GetObjectItem(json, "values");

            if (values && cJSON_IsArray(values)) {
                int arr_size = cJSON_GetArraySize(values);
                entry.count = count_item ? (uint16_t)count_item->valueint : (uint16_t)arr_size;
                if (entry.count > (uint16_t)arr_size) entry.count = (uint16_t)arr_size;

                entry.values = (uint16_t*)malloc(entry.count * sizeof(uint16_t));
                if (entry.values) {
                    for (int i = 0; i < entry.count; i++) {
                        cJSON* v = cJSON_GetArrayItem(values, i);
                        entry.values[i] = (v && cJSON_IsNumber(v)) ? (uint16_t)v->valueint : 0;
                    }
                    s_entries.push_back(entry);
                    parsed++;
                }
            }
        }

        cJSON_Delete(json);
        line_start = line_end ? line_end + 1 : line_start + line_len;
    }

    if (parsed > 0) {
        s_state = State::LOADED;
        ESP_LOGI(TAG, "Loaded %d capture entries (%d lines parsed)", parsed, line_num);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "No valid entries found in %d lines", line_num);
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t start() {
    if (s_state == State::IDLE || s_entries.empty()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_current = 0;
    s_start_time = esp_timer_get_time();
    s_state = State::PLAYING;
    ESP_LOGI(TAG, "Playback started (%lu entries)", (unsigned long)s_entries.size());
    return ESP_OK;
}

void stop() {
    s_state = s_entries.empty() ? State::IDLE : State::LOADED;
    s_current = 0;
    s_start_time = 0;
    ESP_LOGI(TAG, "Playback stopped");
}

void pause() {
    if (s_state == State::PLAYING) {
        s_state = State::PAUSED;
        ESP_LOGI(TAG, "Playback paused at entry %lu", (unsigned long)s_current);
    }
}

void resume() {
    if (s_state == State::PAUSED) {
        s_state = State::PLAYING;
        ESP_LOGI(TAG, "Playback resumed");
    }
}

void tick() {
    if (s_state != State::PLAYING) return;
    if (s_current >= s_entries.size()) {
        ESP_LOGI(TAG, "Playback complete (%lu entries)", (unsigned long)s_entries.size());
        s_state = State::LOADED;
        s_current = 0;
        s_start_time = 0;
        return;
    }

    uint32_t now = elapsedMs();

    // Apply all entries whose timestamp has been reached
    while (s_current < s_entries.size() && s_entries[s_current].timestamp_ms <= now) {
        const Entry& e = s_entries[s_current];
        for (uint16_t i = 0; i < e.count; i++) {
            reg::set(e.addr + i, e.values[i]);
        }
        ESP_LOGD(TAG, "Applied entry %lu: addr=%u count=%u t=%lu",
                 (unsigned long)s_current, e.addr, e.count, (unsigned long)e.timestamp_ms);
        s_current++;
    }
}

Status getStatus() {
    return {
        .state = s_state,
        .total_entries = (uint32_t)s_entries.size(),
        .current_entry = s_current,
        .elapsed_ms = (s_state == State::PLAYING) ? elapsedMs() : 0,
    };
}

}  // namespace playback
