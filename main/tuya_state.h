#pragma once

// ---------------------------------------------------------------------------
// Per-window byte store for the Tuya MCU wire protocol.
//
// The Arctic heat pump's wire protocol is byte-framed: each request maps to
// a fixed (field_a, field_b) "window" whose response is exactly field_b
// payload bytes. Each wire byte corresponds to one Arctic "register" (with
// an optional static prefix at the start of some windows).
//
// This module stores the canonical state as raw bytes per window. The
// existing uint16_t register_map[] is, in time, going to become a
// projection over this byte store rather than the source of truth. That
// migration happens in a later phase; for now the two coexist and this
// module is only exercised by tests and the upcoming tuya_slave.
//
// Concurrency: snapshot() copies the entire window payload under a mutex
// so the caller can release the lock before doing UART I/O. Multiple
// writers (playback, API, simulation, the slave's own write-handler) are
// expected; all writes go through this module.
//
// Pure C++17 with no ESP-IDF includes in the header, so the module
// compiles natively for host-side unit tests. The .cpp picks the right
// mutex implementation at build time.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>

namespace tuya_state {

// Maximum number of distinct windows the store can hold. The codec
// currently knows about 2, but leave headroom for write-channel windows
// observed in future captures.
constexpr size_t MAX_WINDOWS = 8;

// Maximum payload bytes for any one window. The largest window currently
// known is 58 bytes (holding regs); cap is generous for the same reason
// as MAX_WINDOWS.
constexpr size_t MAX_WINDOW_BYTES = 256;

// Initialize the store from tuya_codec::KNOWN_WINDOWS. Idempotent —
// repeated calls are no-ops. Each window starts zero-filled.
void init();

// Test-only: forget all windows. Lets tests re-init from scratch.
void resetForTest();

// True once init() has been called.
bool isInitialized();

// Number of registered windows (after init).
size_t windowCount();

// ---------------------------------------------------------------------------
// Read API
// ---------------------------------------------------------------------------

// Copy the entire payload of window (field_a, field_b) into out_buf.
// Acquires the mutex internally; the caller may safely release control
// and use out_buf without further synchronization.
//
// Returns false if the window is unknown, the size doesn't match the
// registered window's field_b, or out_buf_capacity is too small.
bool snapshot(uint16_t field_a, uint16_t field_b,
              uint8_t *out_buf, size_t out_buf_capacity);

// Single-byte read. Returns 0 if window unknown or offset out of range.
uint8_t getByte(uint16_t field_a, size_t offset);

// ---------------------------------------------------------------------------
// Write API
// ---------------------------------------------------------------------------

// Replace the entire payload of a window. payload_len MUST equal the
// registered window's field_b (no partial writes; no truncation). The
// strictness is intentional: if a capture line's payload doesn't match
// the expected window size, the playback should reject it rather than
// silently corrupt state.
bool writeWindow(uint16_t field_a, uint16_t field_b,
                 const uint8_t *payload, size_t payload_len);

// Single-byte write. Returns false if window unknown or offset out of
// range.
bool setByte(uint16_t field_a, size_t offset, uint8_t value);

// ---------------------------------------------------------------------------
// register_map compatibility projection
//
// These map an Arctic "register number" (the addresses used by the legacy
// register_map module — 2000..2057 and 2100..2138) into a single byte
// inside the corresponding window, using tuya_codec::KNOWN_WINDOWS to
// resolve the window and prefix offset.
//
// projectGet returns the byte as a zero-extended uint16_t, matching the
// shape of the old register_map::get(). Registers that span multiple
// bytes (the 16-bit status bitmap at reg 2135, for example) are not yet
// addressed by this projection; they will need explicit per-register
// metadata in a later phase.
// ---------------------------------------------------------------------------

// Look up the byte that backs `reg_addr` and return it as a uint16_t.
// Returns 0 if reg_addr does not map into any registered window.
uint16_t projectGet(uint16_t reg_addr);

// Write the low byte of `value` into the byte that backs `reg_addr`.
// Returns false if reg_addr does not map into any registered window.
bool projectSet(uint16_t reg_addr, uint16_t value);

// True if reg_addr maps to a byte inside a registered window.
bool projectKnows(uint16_t reg_addr);

}  // namespace tuya_state
