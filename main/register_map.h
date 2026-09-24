/*
 * Simulator presets + raw register debug access.
 *
 * The simulator holds NO register map of its own: every address, bit and
 * scaling lives in the arctic-macon library (macon_fields.h / macon_image.h).
 * Presets are described by named fields, and the only register-number API
 * left here is the raw debug passthrough behind /api/registers.
 */
#pragma once

#include <stdint.h>
#if defined(ESP_PLATFORM)
  #include "esp_err.h"
#else
  // Host unit-test build: minimal stand-ins for the ESP-IDF error type.
  typedef int esp_err_t;
  #ifndef ESP_OK
    #define ESP_OK 0
  #endif
  #ifndef ESP_ERR_NOT_FOUND
    #define ESP_ERR_NOT_FOUND 0x105
  #endif
#endif

namespace reg {

enum class Preset {
    IDLE,
    HEATING,
    COOLING,
    HOT_WATER,
    DEFROST,
    FAULT_P01,
};

// Seed the store with the OEM baseline and load the IDLE preset.
void init();

// Load a preset atomically (one lock for the whole state change).
void loadPreset(Preset preset);

// Parse a preset key ("idle", "heating", ...). Returns false if unknown.
bool presetFromKey(const char *key, Preset *out);
const char *presetKey(Preset preset);

// Clear every fault site, keeping the RUN indicator.
void clearErrors();

// Number of preset field writes the library has rejected since boot. Always 0
// unless a preset table is wrong; host tests assert on it.
int presetRejectCount();

// --- raw debug passthrough (/api/registers) -------------------------------
// Addresses are only accepted if a served Tuya window carries them.
bool      isValid(uint16_t addr);
uint16_t  get(uint16_t addr);
esp_err_t set(uint16_t addr, uint16_t value);

}  // namespace reg
