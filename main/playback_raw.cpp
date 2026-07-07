#include "playback_raw.h"

#include "tuya_codec.h"

#include <cstring>

namespace playback_raw {

namespace {

// Map a single ASCII hex digit to its 0..15 value, or -1 if not a hex digit.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Decode `hex` (length `hex_len`, even) into `out_buf` (capacity bytes).
// Returns the number of bytes written, or 0 on error.
size_t hex_decode(const char *hex, size_t hex_len,
                  uint8_t *out_buf, size_t out_capacity) {
    if ((hex_len % 2) != 0) return 0;
    size_t n = hex_len / 2;
    if (n > out_capacity) return 0;
    for (size_t i = 0; i < n; ++i) {
        int hi = hex_value(hex[2 * i]);
        int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out_buf[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return n;
}

}  // namespace

DecodeResult decode_response(const char *hex, size_t hex_len, RawResponse &out) {
    if (!hex || hex_len == 0 || (hex_len % 2) != 0) {
        return DecodeResult::BAD_HEX;
    }

    // Cap the decode buffer at MAX_FRAME_LEN * 4 — enough to hold a
    // request + response + a few trailing bytes for any window we know.
    uint8_t bytes[tuya_codec::MAX_FRAME_LEN * 4];
    size_t  bytes_len = hex_decode(hex, hex_len, bytes, sizeof(bytes));
    if (bytes_len == 0) {
        return DecodeResult::BAD_HEX;
    }

    // Walk frames until we find a DIR_RESPONSE that parses cleanly.
    size_t off = 0;
    while (off < bytes_len) {
        size_t start = tuya_codec::find_frame_start(bytes + off, bytes_len - off);
        if (start >= bytes_len - off) break;
        off += start;

        tuya_codec::ParsedFrame pf;
        auto pr = tuya_codec::parse_frame(bytes + off, bytes_len - off, pf);
        if (pr != tuya_codec::ParseResult::OK) {
            // Skip past this magic and keep searching — the candidate may
            // be a false-positive header inside another frame's payload.
            off += 1;
            continue;
        }

        if (pf.dir == tuya_codec::DIR_RESPONSE) {
            if (pf.payload_len > MAX_RAW_PAYLOAD) {
                return DecodeResult::PAYLOAD_TOO_LARGE;
            }
            out.field_a     = pf.field_a;
            out.field_b     = pf.field_b;
            out.payload_len = pf.payload_len;
            if (pf.payload_len > 0 && pf.payload != nullptr) {
                std::memcpy(out.payload, pf.payload, pf.payload_len);
            }
            return DecodeResult::OK;
        }

        // Request frame — skip past it and continue looking for a response.
        off += pf.frame_len;
    }

    return DecodeResult::NO_RESPONSE;
}

}  // namespace playback_raw
