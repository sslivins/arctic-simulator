/*
 * Reactive Simulation Engine
 *
 * Maps holding register commands (written by the Modbus master / controller)
 * to input register status (read back by the master) so the simulator
 * mirrors real heat pump behaviour.
 *
 * Key mapping:
 *   UNIT_ON_OFF (2000)  + WORKING_MODE (2001)  →  STATUS_2 (2135)
 *
 * Call updateStatus() after any holding register modification (Modbus write
 * or REST API write).  Preset loads set STATUS_2 explicitly and should NOT
 * be followed by updateStatus() — they already contain the desired state.
 *
 * Simulation can be disabled (e.g. during capture playback) so that STATUS_2
 * values from the capture file are preserved rather than being overwritten.
 */
#pragma once

namespace simulation {

// Enable or disable the reactive simulation engine.
// When disabled, updateStatus() becomes a no-op.
void setEnabled(bool enabled);
bool isEnabled();

// Call after any holding register write to recompute status registers.
// No-op when simulation is disabled.
void updateStatus();

}  // namespace simulation
