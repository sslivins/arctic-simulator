/*
 * Playback Engine for Arctic Simulator
 * Loads JSONL capture files and replays register values
 * timed to match the original capture.
 *
 * Capture format (JSONL — one JSON object per line):
 *   {"t":0,"fc":3,"addr":2100,"count":39,"values":[...]}
 *   {"t":500,"fc":3,"addr":2000,"count":58,"values":[...]}
 *   {"t":1000,"fc":6,"addr":2000,"value":1}
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

namespace playback {

enum class State {
    IDLE,       // No capture loaded
    LOADED,     // Capture loaded, not playing
    PLAYING,    // Playing back
    PAUSED,     // Paused mid-playback
};

struct Status {
    State    state;
    uint32_t total_entries;
    uint32_t current_entry;
    uint32_t elapsed_ms;
};

// Initialize playback subsystem
void init();

// Load capture data from a JSONL string (typically received via API)
esp_err_t loadFromString(const char* jsonl_data);

// Start/stop/pause playback
esp_err_t start();
void stop();
void pause();
void resume();

// Process playback tick — call periodically from main loop
// Applies register values whose timestamp has been reached
void tick();

// Get current status
Status getStatus();

}  // namespace playback
