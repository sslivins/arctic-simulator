// ---------------------------------------------------------------------------
// Unit tests for playback_raw — decoding sniffer "raw" jsonl lines into
// (field_a, field_b, payload) triples.
// ---------------------------------------------------------------------------

#include "playback_raw.h"
#include "tuya_codec/tuya_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                        \
                         __FILE__, __LINE__, #expr);                       \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

std::string to_hex(const uint8_t *b, size_t n) {
    static const char H[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(H[b[i] >> 4]);
        s.push_back(H[b[i] & 0xf]);
    }
    return s;
}

// Build request+response hex blob for a known window. Optional trailing
// bytes simulate the sniffer's inter-frame silence artifact.
std::string build_blob(uint16_t field_a, uint16_t field_b,
                       const std::vector<uint8_t> &payload,
                       const std::vector<uint8_t> &trailing = {}) {
    uint8_t req[tuya_codec::MAX_FRAME_LEN];
    size_t  req_len = tuya_codec::encode_request(req, sizeof(req),
                                                 tuya_codec::FC_READ,
                                                 field_a, field_b);
    uint8_t resp[tuya_codec::MAX_FRAME_LEN];
    size_t  resp_len = tuya_codec::encode_response(resp, sizeof(resp),
                                                   tuya_codec::FC_READ,
                                                   field_a, field_b,
                                                   payload.data());

    std::string out;
    out += to_hex(req, req_len);
    out += to_hex(resp, resp_len);
    if (!trailing.empty()) out += to_hex(trailing.data(), trailing.size());
    return out;
}

void test_telemetry_roundtrip() {
    std::vector<uint8_t> payload(50);
    for (size_t i = 0; i < 50; ++i) payload[i] = static_cast<uint8_t>(i + 1);
    std::string hex = build_blob(0, 50, payload);

    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(hex.data(), hex.size(), rr);
    CHECK(rc == playback_raw::DecodeResult::OK);
    CHECK(rr.field_a == 0);
    CHECK(rr.field_b == 50);
    CHECK(rr.payload_len == 50);
    CHECK(std::memcmp(rr.payload, payload.data(), 50) == 0);
}

void test_holding_roundtrip() {
    std::vector<uint8_t> payload(58);
    for (size_t i = 0; i < 58; ++i) payload[i] = static_cast<uint8_t>(0x80 + i);
    std::string hex = build_blob(50, 58, payload);

    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(hex.data(), hex.size(), rr);
    CHECK(rc == playback_raw::DecodeResult::OK);
    CHECK(rr.field_a == 50);
    CHECK(rr.field_b == 58);
    CHECK(rr.payload_len == 58);
    CHECK(std::memcmp(rr.payload, payload.data(), 58) == 0);
}

void test_tolerates_trailing_bytes() {
    std::vector<uint8_t> payload(58, 0x42);
    std::string hex = build_blob(50, 58, payload, {0x00, 0x14});

    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(hex.data(), hex.size(), rr);
    CHECK(rc == playback_raw::DecodeResult::OK);
    CHECK(rr.payload_len == 58);
}

void test_rejects_bad_hex() {
    const char *bad = "55aaXX";
    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(bad, std::strlen(bad), rr);
    CHECK(rc == playback_raw::DecodeResult::BAD_HEX);
}

void test_rejects_odd_length_hex() {
    const char *bad = "55aaf";
    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(bad, std::strlen(bad), rr);
    CHECK(rc == playback_raw::DecodeResult::BAD_HEX);
}

void test_no_response_when_only_request() {
    uint8_t req[tuya_codec::MIN_FRAME_LEN];
    size_t  req_len = tuya_codec::encode_request(req, sizeof(req),
                                                 tuya_codec::FC_READ, 0, 50);
    std::string hex = to_hex(req, req_len);

    playback_raw::RawResponse rr{};
    auto rc = playback_raw::decode_response(hex.data(), hex.size(), rr);
    CHECK(rc == playback_raw::DecodeResult::NO_RESPONSE);
}

}  // namespace

int main() {
    test_telemetry_roundtrip();
    test_holding_roundtrip();
    test_tolerates_trailing_bytes();
    test_rejects_bad_hex();
    test_rejects_odd_length_hex();
    test_no_response_when_only_request();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return 1;
    }
    std::printf("All playback_raw tests passed.\n");
    return 0;
}
