/*
 * Local Register Recorder
 *
 * Periodically captures all Modbus register values and writes them
 * to a JSONL file on the SPIFFS partition.  The resulting file is
 * directly compatible with the playback engine.
 *
 * Storage lives on the "storage" SPIFFS partition which is 2 MB on
 * the 4 MB Atom S3 and ~6 MB on the 8 MB Atom S3R.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

namespace recorder {

struct Status {
    bool     recording;
    uint32_t entries;
    uint32_t elapsed_ms;
    size_t   bytes_used;   // bytes written to current file
    size_t   bytes_total;  // total SPIFFS capacity
};

// Mount SPIFFS and prepare the subsystem.
esp_err_t init();

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

// Delete the recording file to reclaim storage.
esp_err_t deleteData();

// Filesystem path of the recording file (for API download).
const char* getFilePath();

}  // namespace recorder
