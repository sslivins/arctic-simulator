// ---------------------------------------------------------------------------
// Unit tests for tuya_state.
//
// Hand-rolled asserts (no test framework dependency) — mirrors the
// arctic-sniffer pattern. A failed CHECK prints file:line and exits 1.
// ---------------------------------------------------------------------------

#include "tuya_state.h"
#include "tuya_codec/tuya_codec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

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

void test_init_registers_known_windows() {
    tuya_state::resetForTest();
    CHECK(!tuya_state::isInitialized());
    CHECK(tuya_state::windowCount() == 0);

    tuya_state::init();
    CHECK(tuya_state::isInitialized());
    CHECK(tuya_state::windowCount() == tuya_codec::KNOWN_WINDOWS_COUNT);

    // Repeat init is a no-op.
    tuya_state::init();
    CHECK(tuya_state::windowCount() == tuya_codec::KNOWN_WINDOWS_COUNT);
}

void test_init_zero_fills_windows() {
    tuya_state::resetForTest();
    tuya_state::init();

    uint8_t buf[256];
    std::memset(buf, 0xAB, sizeof(buf));
    CHECK(tuya_state::snapshot(0, 50, buf, sizeof(buf)));
    for (size_t i = 0; i < 50; ++i) CHECK(buf[i] == 0x00);

    std::memset(buf, 0xAB, sizeof(buf));
    CHECK(tuya_state::snapshot(50, 58, buf, sizeof(buf)));
    for (size_t i = 0; i < 58; ++i) CHECK(buf[i] == 0x00);
}

void test_snapshot_rejects_unknown_window() {
    tuya_state::resetForTest();
    tuya_state::init();

    uint8_t buf[256];
    // Wrong field_b for known field_a=0.
    CHECK(!tuya_state::snapshot(0, 49, buf, sizeof(buf)));
    // Totally unknown field_a.
    CHECK(!tuya_state::snapshot(99, 10, buf, sizeof(buf)));
    // Capacity too small.
    CHECK(!tuya_state::snapshot(0, 50, buf, 49));
    // Null buffer.
    CHECK(!tuya_state::snapshot(0, 50, nullptr, 50));
}

void test_write_window_round_trip() {
    tuya_state::resetForTest();
    tuya_state::init();

    uint8_t payload[50];
    for (size_t i = 0; i < 50; ++i) payload[i] = static_cast<uint8_t>(i + 1);
    CHECK(tuya_state::writeWindow(0, 50, payload, 50));

    uint8_t out[50];
    CHECK(tuya_state::snapshot(0, 50, out, sizeof(out)));
    CHECK(std::memcmp(out, payload, 50) == 0);

    // The other window is untouched.
    uint8_t holding[58];
    CHECK(tuya_state::snapshot(50, 58, holding, sizeof(holding)));
    for (size_t i = 0; i < 58; ++i) CHECK(holding[i] == 0x00);
}

void test_write_window_rejects_size_mismatch() {
    tuya_state::resetForTest();
    tuya_state::init();

    uint8_t payload[50] = {};
    CHECK(!tuya_state::writeWindow(0, 49, payload, 49));        // bad field_b
    CHECK(!tuya_state::writeWindow(0, 50, payload, 49));        // bad length
    CHECK(!tuya_state::writeWindow(0, 50, nullptr, 50));        // null payload
    CHECK(!tuya_state::writeWindow(7, 50, payload, 50));        // unknown
}

void test_set_get_byte() {
    tuya_state::resetForTest();
    tuya_state::init();

    CHECK(tuya_state::setByte(0, 7, 0x42));
    CHECK(tuya_state::getByte(0, 7) == 0x42);

    // Out-of-range
    CHECK(!tuya_state::setByte(0, 999, 0xFF));
    CHECK(tuya_state::getByte(0, 999) == 0x00);

    // Unknown window
    CHECK(!tuya_state::setByte(123, 0, 0xFF));
    CHECK(tuya_state::getByte(123, 0) == 0x00);
}

void test_projection_telemetry_window() {
    tuya_state::resetForTest();
    tuya_state::init();

    // Telemetry: field_a=0, field_b=50, reg_base=2100, prefix=7.
    // reg 2100 maps to byte 7 of the window.
    CHECK(tuya_state::projectKnows(2100));
    CHECK(tuya_state::projectKnows(2142));   // 2100 + (50 - 7) - 1 = 2142
    CHECK(!tuya_state::projectKnows(2099));
    CHECK(!tuya_state::projectKnows(2143));

    CHECK(tuya_state::projectSet(2100, 0x37));
    CHECK(tuya_state::projectGet(2100) == 0x37);
    CHECK(tuya_state::getByte(0, 7) == 0x37);

    CHECK(tuya_state::projectSet(2110, 20));
    CHECK(tuya_state::projectGet(2110) == 20);
    CHECK(tuya_state::getByte(0, 17) == 20);  // 7 + (2110 - 2100)
}

void test_projection_holding_window() {
    tuya_state::resetForTest();
    tuya_state::init();

    // Holding: field_a=50, field_b=58, reg_base=2000, prefix=0.
    CHECK(tuya_state::projectKnows(2000));
    CHECK(tuya_state::projectKnows(2057));
    CHECK(!tuya_state::projectKnows(2058));

    CHECK(tuya_state::projectSet(2000, 0xAB));
    CHECK(tuya_state::getByte(50, 0) == 0xAB);

    CHECK(tuya_state::projectSet(2057, 0xCD));
    CHECK(tuya_state::getByte(50, 57) == 0xCD);

    // Low-byte-only behaviour: high bits are discarded.
    CHECK(tuya_state::projectSet(2010, 0x1234));
    CHECK(tuya_state::projectGet(2010) == 0x34);
}

void test_projection_unknown_register() {
    tuya_state::resetForTest();
    tuya_state::init();

    CHECK(tuya_state::projectGet(9999) == 0);
    CHECK(!tuya_state::projectSet(9999, 0xFF));
}

void test_uses_codec_window_table() {
    // Every window declared by the codec must be addressable through
    // tuya_state after init(). Catches future codec additions that
    // forget to extend MAX_WINDOWS.
    tuya_state::resetForTest();
    tuya_state::init();
    for (size_t i = 0; i < tuya_codec::KNOWN_WINDOWS_COUNT; ++i) {
        const auto &w = tuya_codec::KNOWN_WINDOWS[i];
        uint8_t buf[256];
        CHECK(tuya_state::snapshot(w.field_a, w.field_b, buf, sizeof(buf)));
    }
}

}  // namespace

int main() {
    test_init_registers_known_windows();
    test_init_zero_fills_windows();
    test_snapshot_rejects_unknown_window();
    test_write_window_round_trip();
    test_write_window_rejects_size_mismatch();
    test_set_get_byte();
    test_projection_telemetry_window();
    test_projection_holding_window();
    test_projection_unknown_register();
    test_uses_codec_window_table();

    if (g_failures > 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return 1;
    }
    std::printf("All tuya_state tests passed.\n");
    return 0;
}
