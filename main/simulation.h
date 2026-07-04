/*
 * Reactive Simulation Engine (Macon: inert)
 *
 * On the ECO-600 this derived the 16-bit STATUS_2 bitmap from the commanded
 * UNIT_ON_OFF (2000) + WORKING_MODE (2001) holding registers. The Macon
 * layout has no such command registers — the status byte (reg 2130) is
 * authored directly by presets and the REST API — so updateStatus() is now a
 * no-op. The enable/disable API is retained for source compatibility with
 * existing callers and the /api/simulation endpoint.
 */
#pragma once

namespace simulation {

// Enable or disable the reactive simulation engine (currently inert).
void setEnabled(bool enabled);
bool isEnabled();

// No-op on the Macon layout — status is set directly, not derived.
void updateStatus();

}  // namespace simulation
