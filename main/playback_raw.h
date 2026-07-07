#pragma once

// ---------------------------------------------------------------------------
// Helper: decode a sniffer "raw" jsonl line into a (field_a, field_b, payload)
// triple that playback can replay through tuya_state::writeWindow.
//
// The sniffer's raw lines look like:
//
//   {"t":<ms>,"raw":"<hex>"}
//
// where <hex> is the concatenation of the controller's REQUEST frame and
// the heat pump's RESPONSE frame as captured on the wire (and sometimes
// followed by 1-2 bytes of inter-frame silence the sniffer logged as wire
// bytes — those are tolerated).
//
// We are only interested in the response, because that's the snapshot of
// the heat pump's state. This helper:
//   1. decodes the hex into bytes,
//   2. walks the byte stream looking for the first DIR_RESPONSE frame,
//   3. validates its checksum via tuya_codec::parse_frame,
//   4. fills out (field_a, field_b, payload bytes).
//
// Pure C++17; no ESP-IDF; safe to unit-test on the host.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

namespace playback_raw {

constexpr size_t MAX_RAW_PAYLOAD = 256;

struct RawResponse {
    uint16_t field_a;
    uint16_t field_b;
    uint8_t  payload[MAX_RAW_PAYLOAD];
    size_t   payload_len;
};

enum class DecodeResult {
    OK,
    BAD_HEX,            // hex string contains a non-hex character or odd length
    NO_RESPONSE,        // walked the stream and never found a DIR_RESPONSE frame
    PAYLOAD_TOO_LARGE,  // response payload would not fit in RawResponse::payload
};

/// Decode `hex` (length `hex_len`, must be even) into bytes, find the first
/// Tuya response frame, validate it, and populate `out`. On any error, `out`
/// is left in an unspecified state.
DecodeResult decode_response(const char *hex, size_t hex_len, RawResponse &out);

}  // namespace playback_raw
