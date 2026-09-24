#pragma once

// ---------------------------------------------------------------------------
// tuya_state — the simulator's single source of heat-pump state.
//
// All state lives in ONE arctic::MaconImage (from the arctic-macon library)
// guarded by a mutex. Every consumer goes through it:
//
//   * tuya_slave serves reads with snapshot() (-> MaconImage::read_window) and
//     applies controller fc=0x06 writes with applyWrite()
//     (-> MaconImage::apply_write), exactly as the real unit reflects them.
//   * playback replays captured windows with writeWindow() (-> ingest_bytes).
//   * presets, the semantic REST API and the display use Access to set/decode
//     fields by meaning (macon_fields.h), never by register number.
//   * the raw /api/registers debug endpoint uses projectGet/projectSet.
//
// Because the library owns every register/bit mapping, the simulator and the
// controller cannot drift apart (both report macon_layout_fingerprint()).
//
// Pure C++17 with no ESP-IDF includes in the header, so the module compiles
// natively for host-side unit tests.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>

#include "macon_image.h"

namespace tuya_state {

// Largest payload of any known window (holding = 58 bytes); generous cap.
constexpr size_t MAX_WINDOW_BYTES = 256;

// Initialize the store: an image with every known-window register present
// (value 0). Idempotent — repeated calls are no-ops.
void init();

// Test-only: forget all state. Lets tests re-init from scratch.
void resetForTest();

// True once init() has been called.
bool isInitialized();

// Number of Tuya windows served (tuya_codec::KNOWN_WINDOWS_COUNT after init).
size_t windowCount();

// ---------------------------------------------------------------------------
// Wire-level API (slave, playback)
// ---------------------------------------------------------------------------

// Copy the payload of window (field_a, field_b) into out_buf. Returns false if
// the window is unknown, the store is not initialised or the buffer is small.
bool snapshot(uint16_t field_a, uint16_t field_b,
              uint8_t *out_buf, size_t out_buf_capacity);

// Single payload byte of the window starting at field_a. 0 if unknown.
uint8_t getByte(uint16_t field_a, size_t offset);

// Replace an entire window payload. payload_len MUST equal field_b; a size
// mismatch is rejected so a bad capture line can't corrupt state.
bool writeWindow(uint16_t field_a, uint16_t field_b,
                 const uint8_t *payload, size_t payload_len);

// Single payload byte write into the window starting at field_a.
bool setByte(uint16_t field_a, size_t offset, uint8_t value);

// Apply a controller fc=0x06 write (`len` data bytes at wire address
// `wire_addr`). All-or-nothing; false if any byte is outside a known window.
bool applyWrite(uint16_t wire_addr, const uint8_t *data, size_t len);

// ---------------------------------------------------------------------------
// Raw register debug projection (/api/registers only)
// ---------------------------------------------------------------------------

uint16_t projectGet(uint16_t reg_addr);           // 0 if not served
bool     projectSet(uint16_t reg_addr, uint16_t value);  // low byte stored
bool     projectKnows(uint16_t reg_addr);         // reg is inside a served window

// ---------------------------------------------------------------------------
// Semantic access
// ---------------------------------------------------------------------------

// Exclusive access to the image for a batch of semantic reads/writes (a
// preset, a PATCH /api/state, a decode). Holds the store mutex for its
// lifetime, so keep it short and never call another tuya_state function while
// one is alive (the mutex is not recursive).
class Access {
public:
    Access();
    ~Access();
    Access(const Access &)            = delete;
    Access &operator=(const Access &) = delete;
    arctic::MaconImage &image();
};

// Convenience: decode the current state under the lock.
void decode(arctic::MaconState *out);

// Monotonic counter bumped on every mutation; lets pollers (display, SSE)
// skip work when nothing changed.
uint32_t generation();

}  // namespace tuya_state
