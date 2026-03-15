/*
 * Reactive Simulation Engine — Implementation
 *
 * Translates holding register commands into input register status to
 * emulate real ECO-600 heat pump behaviour.
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
    if (!s_enabled) return;
    uint16_t unit_on = reg::get(reg::UNIT_ON_OFF);
    uint16_t mode    = reg::get(reg::WORKING_MODE);

    uint16_t sts2 = 0;

    if (unit_on) {
        // Core status — always on when unit is running
        sts2 |= reg::STS2_UNIT_ON;
        sts2 |= reg::STS2_REMOTE_ON;
        sts2 |= reg::STS2_WATER_PUMP;
        sts2 |= reg::STS2_WATER_FLOW;
        sts2 |= reg::STS2_COMPRESSOR;

        // Mode-specific peripherals
        switch (mode) {
        case reg::MODE_COOLING:
            sts2 |= reg::STS2_FAN_MED;
            sts2 |= reg::STS2_4WAY_VALVE;   // Reversing valve for cooling
            break;

        case reg::MODE_FLOOR_HEATING:
            sts2 |= reg::STS2_FAN_LOW;
            break;

        case reg::MODE_FAN_COIL_HEAT:
            sts2 |= reg::STS2_FAN_MED;
            break;

        case reg::MODE_HOT_WATER:
            sts2 |= reg::STS2_FAN_LOW;
            sts2 |= reg::STS2_3WAY_V1;      // DHW circuit valve
            break;

        case reg::MODE_AUTO:
        default:
            sts2 |= reg::STS2_FAN_LOW;
            break;
        }
    }
    // When unit_on == 0, sts2 stays 0 — everything off

    // Only update if the computed status differs (avoids log noise)
    uint16_t current = reg::get(reg::STATUS_2);
    if (current != sts2) {
        reg::set(reg::STATUS_2, sts2);
        ESP_LOGI(TAG, "STATUS_2 updated: 0x%04X → 0x%04X (unit=%u, mode=%u)",
                 current, sts2, unit_on, mode);
    }
}

}  // namespace simulation
