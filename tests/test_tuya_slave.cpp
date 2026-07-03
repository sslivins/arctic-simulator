// Native-host tests for the tuya_slave handler core.
//
// Drives the pure `handleBytesForTest` seam — no UART, no FreeRTOS. Seeds
// tuya_state, encodes a request via tuya_codec, pushes it through the
// handler, and asserts byte-exact responses + counter updates.

#include "tuya_slave.h"
#include "tuya_state.h"
#include "tuya_codec/tuya_codec.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t WIN_TEL_A = 0;
constexpr uint16_t WIN_TEL_B = 50;
constexpr uint16_t WIN_HOLD_A = 50;
constexpr uint16_t WIN_HOLD_B = 58;

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do {                                                   \
    if (cond) { ++g_pass; }                                                \
    else { ++g_fail; std::printf("FAIL %s:%d  %s\n",                       \
                                 __FILE__, __LINE__, #cond); }             \
} while (0)

void reset() {
    tuya_state::resetForTest();
    tuya_state::init();
    tuya_slave::resetStats();
}

// Build a 9-byte request frame for the given window.
std::vector<uint8_t> makeRequest(uint16_t a, uint16_t b) {
    std::vector<uint8_t> buf(tuya_codec::MIN_FRAME_LEN);
    size_t n = tuya_codec::encode_request(buf.data(), buf.size(),
                                          tuya_codec::FC_READ, a, b);
    buf.resize(n);
    return buf;
}

void test_unknown_window_unreachable_via_seam() {
    // find_frame_start pre-screens for known windows, so an unknown
    // (field_a, field_b) never reaches the unknown_windows counter via
    // handleBytesForTest. Document that behavior: the frame is silently
    // discarded.
    reset();
    std::vector<uint8_t> req{
        0x55, 0xAA, tuya_codec::DIR_REQUEST, tuya_codec::FC_READ,
        0x00, 0x01,  // field_a = 1 (unknown)
        0x00, 0x32,  // field_b = 50
        0x00,
    };
    uint8_t sum = 0;
    for (size_t i = 2; i <= 7; ++i) sum += req[i];
    req[8] = (uint8_t)(~sum & 0xFF);

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size(),
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == req.size());
    CHECK(out_len == 0);
    auto s = tuya_slave::getStats();
    CHECK(s.frames_seen == 0);
    CHECK(s.responses_sent == 0);
}

void test_telemetry_response_byte_exact() {
    reset();
    // Seed bytes [0..6] (prefix) + bytes [7..49] (reg 2100..2142).
    uint8_t payload[50];
    for (size_t i = 0; i < 50; ++i) payload[i] = (uint8_t)(0x10 + i);
    CHECK(tuya_state::writeWindow(WIN_TEL_A, WIN_TEL_B, payload, 50));

    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);
    CHECK(req.size() == tuya_codec::MIN_FRAME_LEN);

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size(),
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == req.size());
    // 8 hdr + 50 payload + 1 chk = 59
    CHECK(out_len == tuya_codec::HDR_LEN + WIN_TEL_B + tuya_codec::CHK_LEN);
    CHECK(out[0] == 0x55 && out[1] == 0xAA);
    CHECK(out[2] == tuya_codec::DIR_RESPONSE);
    CHECK(out[3] == tuya_codec::FC_READ);
    // Verify payload was copied verbatim from the byte store.
    CHECK(std::memcmp(out + tuya_codec::HDR_LEN, payload, WIN_TEL_B) == 0);

    // Verify the response we just produced parses cleanly back.
    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(out, out_len, pf);
    CHECK(pr == tuya_codec::ParseResult::OK);
    CHECK(pf.dir == tuya_codec::DIR_RESPONSE);
    CHECK(pf.field_a == WIN_TEL_A);
    CHECK(pf.field_b == WIN_TEL_B);

    auto s = tuya_slave::getStats();
    CHECK(s.frames_seen == 1);
    CHECK(s.requests_handled == 1);
    CHECK(s.responses_sent == 1);
    CHECK(s.unknown_windows == 0);
    CHECK(s.parse_errors == 0);
}

void test_holding_response_byte_exact() {
    reset();
    uint8_t payload[58];
    for (size_t i = 0; i < 58; ++i) payload[i] = (uint8_t)(0xA0 + i);
    CHECK(tuya_state::writeWindow(WIN_HOLD_A, WIN_HOLD_B, payload, 58));

    auto req = makeRequest(WIN_HOLD_A, WIN_HOLD_B);
    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size(),
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == req.size());
    CHECK(out_len == tuya_codec::HDR_LEN + WIN_HOLD_B + tuya_codec::CHK_LEN);
    CHECK(std::memcmp(out + tuya_codec::HDR_LEN, payload, WIN_HOLD_B) == 0);

    auto s = tuya_slave::getStats();
    CHECK(s.responses_sent == 1);
}

void test_bad_checksum_counts_parse_error() {
    reset();
    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);
    req.back() ^= 0xFF;   // flip checksum

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size(),
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == req.size());
    CHECK(out_len == 0);
    auto s = tuya_slave::getStats();
    CHECK(s.parse_errors == 1);
    CHECK(s.responses_sent == 0);
    CHECK(s.requests_handled == 0);
    CHECK(s.frames_seen == 0);
}

void test_response_frame_is_ignored() {
    reset();
    // First, seed and produce a real response — then feed THAT response
    // back into the handler. It should be parsed (frames_seen += 1) but
    // no further response should be emitted.
    uint8_t payload[50] = {};
    tuya_state::writeWindow(WIN_TEL_A, WIN_TEL_B, payload, 50);
    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);

    uint8_t rsp[tuya_codec::MAX_FRAME_LEN];
    size_t rsp_len = 0;
    tuya_slave::handleBytesForTest(req.data(), req.size(),
                                   rsp, sizeof(rsp), &rsp_len);
    CHECK(rsp_len > 0);
    tuya_slave::resetStats();

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(rsp, rsp_len,
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == rsp_len);
    CHECK(out_len == 0);
    auto s = tuya_slave::getStats();
    CHECK(s.frames_seen == 1);
    CHECK(s.responses_sent == 0);
}

void test_junk_prefix_is_skipped() {
    reset();
    uint8_t payload[50] = {};
    tuya_state::writeWindow(WIN_TEL_A, WIN_TEL_B, payload, 50);
    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);

    std::vector<uint8_t> buf{0x00, 0x12, 0x34};   // 3 bytes of junk
    buf.insert(buf.end(), req.begin(), req.end());

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(buf.data(), buf.size(),
                                                     out, sizeof(out), &out_len);
    // First call advances past the junk; out_len stays 0.
    CHECK(consumed == 3);
    CHECK(out_len == 0);

    // Second call now sees the real frame at offset 0.
    size_t consumed2 = tuya_slave::handleBytesForTest(buf.data() + consumed,
                                                      buf.size() - consumed,
                                                      out, sizeof(out), &out_len);
    CHECK(consumed2 == req.size());
    CHECK(out_len > 0);
}

void test_truncated_buffer_waits() {
    reset();
    uint8_t payload[50] = {};
    tuya_state::writeWindow(WIN_TEL_A, WIN_TEL_B, payload, 50);
    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);

    uint8_t out[tuya_codec::MAX_FRAME_LEN];
    size_t out_len = 0;
    // Feed less than a full request frame.
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size() - 1,
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == 0);
    CHECK(out_len == 0);
}

void test_response_buffer_too_small() {
    reset();
    uint8_t payload[50] = {};
    tuya_state::writeWindow(WIN_TEL_A, WIN_TEL_B, payload, 50);
    auto req = makeRequest(WIN_TEL_A, WIN_TEL_B);

    uint8_t out[10];   // way too small for a 59-byte response
    size_t out_len = 0;
    size_t consumed = tuya_slave::handleBytesForTest(req.data(), req.size(),
                                                     out, sizeof(out), &out_len);
    CHECK(consumed == req.size());
    CHECK(out_len == 0);
    auto s = tuya_slave::getStats();
    CHECK(s.tx_truncated == 1);
    CHECK(s.responses_sent == 0);
}

}  // namespace

int main() {
    test_unknown_window_unreachable_via_seam();
    test_telemetry_response_byte_exact();
    test_holding_response_byte_exact();
    test_bad_checksum_counts_parse_error();
    test_response_frame_is_ignored();
    test_junk_prefix_is_skipped();
    test_truncated_buffer_waits();
    test_response_buffer_too_small();

    std::printf("tuya_slave: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
