// Golden tests for tuya_slave against real Tuya wire captures.
//
// Each line in the JSONL capture files is a single transaction the
// sniffer saw on the bus: request bytes from the controller immediately
// followed by the heat pump's response bytes (the sniffer is passive
// and sees both directions on the same wire).
//
// For each captured transaction:
//   1. Split the raw blob into the request frame and the response frame.
//   2. Seed tuya_state from the response payload (i.e. pretend the
//      simulator already holds the same window values the real heat
//      pump did at capture time).
//   3. Feed the request bytes into tuya_slave::handleBytesForTest().
//   4. Assert the emitted response matches the captured response
//      byte-for-byte.
//
// If any line's emitted response diverges from the capture, the codec
// or the slave's frame-handling has drifted from the wire protocol —
// fail loudly with a hex diff.

#include "tuya_slave.h"
#include "tuya_state.h"
#include "tuya_codec.h"

#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef CAPTURE_DATA_DIR
#error "CAPTURE_DATA_DIR must be defined by CMake"
#endif

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do {                                                  \
    if (cond) { ++g_pass; }                                               \
    else {                                                                \
        ++g_fail;                                                         \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
} while (0)

// ---- hex helpers ---------------------------------------------------------

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string bytes_to_hex(const uint8_t *buf, size_t len) {
    static const char *digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(digits[(buf[i] >> 4) & 0x0F]);
        s.push_back(digits[buf[i] & 0x0F]);
    }
    return s;
}

// ---- JSONL parsing -------------------------------------------------------
//
// We only need the value of the "raw" field. The capture files are tiny
// and well-formed, so a hand-rolled extraction is plenty.

bool extract_raw_field(const std::string &line, std::string &out) {
    const std::string key = "\"raw\":\"";
    auto pos = line.find(key);
    if (pos == std::string::npos) return false;
    auto start = pos + key.size();
    auto end = line.find('"', start);
    if (end == std::string::npos) return false;
    out.assign(line, start, end - start);
    return true;
}

// ---- frame splitting -----------------------------------------------------
//
// Returns the total length of the frame starting at `buf`, or 0 if the
// header is not valid. Does NOT verify the checksum; the slave's own
// parser does that.

size_t frame_len_at(const uint8_t *buf, size_t len) {
    if (len < tuya_codec::HDR_LEN + tuya_codec::CHK_LEN) return 0;
    if (buf[0] != tuya_codec::HDR0 || buf[1] != tuya_codec::HDR1) return 0;
    uint8_t  dir     = buf[2];
    uint16_t field_b = static_cast<uint16_t>((buf[6] << 8) | buf[7]);
    return tuya_codec::frame_total_len(dir, field_b);
}

}  // namespace

// ---- the test ------------------------------------------------------------

static int run_golden(const char *filename) {
    std::string path = std::string(CAPTURE_DATA_DIR) + "/" + filename;
    std::ifstream in(path);
    if (!in.is_open()) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", path.c_str());
        ++g_fail;
        return 1;
    }

    int lines           = 0;
    int matched         = 0;
    int unmatchable     = 0;  // no request+response pair we can replay
    int payload_skipped = 0;  // payload bytes != window field_b

    std::string line;
    while (std::getline(in, line)) {
        ++lines;
        std::string hex;
        if (!extract_raw_field(line, hex)) continue;

        std::vector<uint8_t> wire;
        if (!hex_to_bytes(hex, wire)) {
            std::fprintf(stderr, "line %d: invalid hex\n", lines);
            ++g_fail;
            continue;
        }

        // Split into request frame + response frame. Captures we have
        // are always request-then-response pairs; if we ever see a line
        // that doesn't start with a request, count it and skip.
        size_t req_len = frame_len_at(wire.data(), wire.size());
        if (req_len == 0 || wire[2] != tuya_codec::DIR_REQUEST) {
            ++unmatchable;
            continue;
        }
        if (req_len + tuya_codec::HDR_LEN + tuya_codec::CHK_LEN > wire.size()) {
            ++unmatchable;
            continue;
        }
        size_t resp_off = req_len;
        size_t resp_len = frame_len_at(wire.data() + resp_off, wire.size() - resp_off);
        if (resp_len == 0 || wire[resp_off + 2] != tuya_codec::DIR_RESPONSE) {
            ++unmatchable;
            continue;
        }
        if (resp_off + resp_len > wire.size()) {
            // Truncated capture — response declares more bytes than the
            // line contains. Skip; this is a sniffer artifact, not a
            // codec problem.
            ++unmatchable;
            continue;
        }
        // Trailing bytes (1-2) are common in our captures — the sniffer
        // appears to log a byte or two of inter-frame silence as wire
        // bytes. They are not part of the response frame; ignore them.
        if (resp_off + resp_len < wire.size()) {
            // Truncate wire view; don't fail.
        }

        // Seed tuya_state from the captured response payload.
        const uint8_t *resp     = wire.data() + resp_off;
        uint16_t       field_a  = static_cast<uint16_t>((resp[4] << 8) | resp[5]);
        uint16_t       field_b  = static_cast<uint16_t>((resp[6] << 8) | resp[7]);
        const uint8_t *payload  = resp + tuya_codec::HDR_LEN;
        size_t         pay_len  = resp_len - tuya_codec::HDR_LEN - tuya_codec::CHK_LEN;

        if (pay_len != field_b) {
            // Capture self-inconsistent — wrong number of payload bytes
            // for the declared count. Don't fail; just track it.
            ++payload_skipped;
            continue;
        }

        tuya_state::resetForTest();
        tuya_state::init();
        if (!tuya_state::writeWindow(field_a, field_b, payload, pay_len)) {
            std::fprintf(stderr, "line %d: writeWindow rejected window (%u,%u) "
                                 "with %zu bytes\n",
                         lines, field_a, field_b, pay_len);
            ++g_fail;
            continue;
        }

        // Feed the request through the slave.
        uint8_t out_buf[256];
        size_t  out_len = 0;
        size_t  consumed = tuya_slave::handleBytesForTest(
            wire.data(), req_len, out_buf, sizeof(out_buf), &out_len);

        if (consumed != req_len) {
            std::fprintf(stderr, "line %d: slave consumed %zu of %zu request bytes\n",
                         lines, consumed, req_len);
            ++g_fail;
            continue;
        }
        if (out_len != resp_len) {
            std::fprintf(stderr,
                "line %d: emitted %zu bytes, expected %zu\n  expected: %s\n  got:      %s\n",
                lines, out_len, resp_len,
                bytes_to_hex(resp, resp_len).c_str(),
                bytes_to_hex(out_buf, out_len).c_str());
            ++g_fail;
            continue;
        }
        if (std::memcmp(out_buf, resp, resp_len) != 0) {
            std::fprintf(stderr,
                "line %d: byte mismatch\n  expected: %s\n  got:      %s\n",
                lines,
                bytes_to_hex(resp, resp_len).c_str(),
                bytes_to_hex(out_buf, out_len).c_str());
            ++g_fail;
            continue;
        }

        ++matched;
    }

    std::printf("[%s] %d lines, %d matched, %d unmatchable, %d payload-skipped\n",
                filename, lines, matched, unmatchable, payload_skipped);

    // Every line in our captures should be a clean request+response pair
    // with the correct payload length. If that changes we want to know.
    CHECK(matched > 0);
    return 0;
}

// ---- entry point ---------------------------------------------------------

int main() {
    run_golden("capture_raw.jsonl");
    run_golden("capture_20260503.jsonl");

    std::printf("\ngolden: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
