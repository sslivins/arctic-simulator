#pragma once

// ---------------------------------------------------------------------------
// Tuya MCU wire-protocol slave.
//
// Drives the RS-485 UART. Decodes inbound request frames using tuya_codec,
// fetches the matching window snapshot from tuya_state, encodes a response
// frame, and writes it back. Owns the RS-485 UART exclusively.
//
// Threading:
//   - One FreeRTOS task pinned to core 1 with its own stack and a UART
//     event queue (same pattern as the sniffer's `sniffer_task`).
//   - The mutex inside tuya_state is acquired only for the duration of a
//     snapshot copy; encode/transmit happen outside the lock.
//
// Statistics are exposed for /api/status; counters are atomic so reads are
// lock-free.
// ---------------------------------------------------------------------------

#if defined(ESP_PLATFORM)
  #include "esp_err.h"
#else
  using esp_err_t = int;
#endif
#include <cstddef>
#include <cstdint>

namespace tuya_slave {

struct Stats {
    uint32_t frames_seen;        // every successfully parsed frame
    uint32_t requests_handled;   // request frames matched to a known window
    uint32_t responses_sent;     // response frames written to UART
    uint32_t parse_errors;       // any non-OK ParseResult from tuya_codec
    uint32_t unknown_windows;    // requests for windows not in KNOWN_WINDOWS
    uint32_t snapshot_failures;  // window was registered but snapshot() returned false
    uint32_t tx_truncated;       // encode_response returned 0 or uart_write_bytes < expected
    uint32_t uart_errors;        // UART_FIFO_OVF / UART_BUFFER_FULL / framing / parity
};

// Initialize the UART and start the slave task. Idempotent.
esp_err_t init();

// Tear the slave down (frees the UART). Safe to call before init().
void deinit();

bool isInitialized();

Stats getStats();
void  resetStats();

// ---------------------------------------------------------------------------
// Test seam (not part of the runtime UART loop).
//
// Drives the pure frame-handling path with a caller-provided byte buffer
// (no UART). Returns the number of bytes consumed from `in` (>= 0) so the
// caller can advance its cursor. If the request was handled, writes the
// response into `out` and sets *out_len. If the inbound bytes were a
// response, an unknown window, a parse error, or simply not enough bytes,
// *out_len is set to 0 (counters are still updated). Returns 0 when there
// is no complete frame at the head of `in` yet.
//
// Statistics counters are updated exactly as the live task would update
// them.
// ---------------------------------------------------------------------------
size_t handleBytesForTest(const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t out_capacity,
                          size_t *out_len);

}  // namespace tuya_slave
