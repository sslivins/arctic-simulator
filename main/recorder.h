/*
 * Register Recorder (PSRAM)
 *
 * On devices with PSRAM (Atom S3R) this allocates a memory buffer
 * and writes periodic register snapshots in JSONL format.  The data
 * can be downloaded via the REST API.
 *
 * On devices without PSRAM (Atom S3) the recorder is unavailable;
 * the web interface should stream registers directly to the client.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

namespace recorder {

struct Status {
    bool     recording;
    bool     available;    // true when PSRAM is present
    uint32_t entries;
    uint32_t elapsed_ms;
    size_t   bytes_used;   // bytes written to buffer
    size_t   bytes_total;  // buffer capacity
};

// Probe PSRAM, allocate buffer.  Safe to call on any variant —
// returns ESP_ERR_NOT_SUPPORTED when no PSRAM is found.
esp_err_t init();

// True if the device has PSRAM and the buffer was allocated.
bool isAvailable();

// Start / stop recording.
esp_err_t start();
void stop();

// Is a recording in progress?
bool isRecording();

// Call periodically (every ~500 ms) from a task.
// When recording, captures a full register snapshot.
void tick();

// Current status.
Status getStatus();

// Discard recorded data.
void clear();

// Access the raw buffer for download (pointer + length).
const uint8_t* bufferData();
size_t          bufferSize();

}  // namespace recorder
