/*
 * Reactive Simulation Engine — Implementation
 *
 * On the ECO-600 this derived the 16-bit STATUS_2 bitmap from the commanded
 * unit-on/working-mode holding registers. The Macon layout has no such
 * command registers — status is a single byte (reg 2130) that presets and the
 * REST API set directly — so there is nothing to reactively derive. The
 * enable/disable API is retained for compatibility; updateStatus() is a no-op.
 */
#include "simulation.h"
#include "register_map.h"
#include "esp_log.h"

static const char* TAG = "sim";

namespace simulation {

static bool s_enabled = true;

void setEnabled(bool enabled) {
    if (s_enabled != enabled) {
        ESP_LOGI(TAG, "Simulation %s", enabled ? "enabled" : "disabled");
    }
    s_enabled = enabled;
}

bool isEnabled() { return s_enabled; }

void updateStatus() {
    // No reactive derivation on the Macon layout — status byte (reg 2130) is
    // authored directly via presets / the REST API.
}

}  // namespace simulation

