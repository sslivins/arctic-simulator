// ---------------------------------------------------------------------------
// Presets + semantic state tests.
//
// Everything is asserted through the arctic-macon library's named fields and
// decode (the same decode the controller runs), never by register number, so
// the simulator can't silently drift from the library.
// ---------------------------------------------------------------------------

#include "register_map.h"
#include "tuya_state.h"
#include "tuya_codec.h"
#include "macon_fields.h"
#include "macon_faults.h"
#include "macon_state.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

using namespace arctic;

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

#define CHECK_FIELD(name, want)                                            \
    do {                                                                   \
        const int32_t got_ = field(name);                                  \
        if (got_ != (want)) {                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n",        \
                         __FILE__, __LINE__, name, (long)got_, (long)(want)); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

int32_t field(const char *name) {
    const MaconFieldDesc *d = macon_field_find(name);
    if (!d) { std::fprintf(stderr, "unknown field %s\n", name); ++g_failures; return -9999; }
    MaconState s{};
    tuya_state::decode(&s);
    int32_t v = -9999;
    if (!macon_field_get(s, *d, &v)) {
        std::fprintf(stderr, "field %s not decodable\n", name);
        ++g_failures;
    }
    return v;
}

size_t activeFaults(MaconFault *out, size_t max) {
    MaconState s{};
    tuya_state::decode(&s);
    return macon_decode_faults(s.fault_run, s.fault_ee, s.fault_comp,
                               s.fault_elec, s.fault_ref, out, max);
}

void fresh() {
    tuya_state::resetForTest();
    reg::init();
}

void test_init_is_idle_with_no_faults() {
    fresh();
    CHECK(reg::presetRejectCount() == 0);
    CHECK_FIELD("compressor_freq", 0);
    CHECK_FIELD("pump_on", 0);
    CHECK_FIELD("working_mode", (int32_t)MaconWorkingMode::HotWater);
    MaconFault f[8];
    CHECK(activeFaults(f, 8) == 0);
    MaconState s{};
    tuya_state::decode(&s);
    CHECK(std::strcmp(operation_name(decode_operation(s)), "Idle") == 0);
}

void test_every_preset_applies_cleanly() {
    fresh();
    const char *keys[] = {"idle", "heating", "cooling", "hot_water", "defrost", "fault_p01"};
    for (const char *k : keys) {
        reg::Preset p;
        CHECK(reg::presetFromKey(k, &p));
        CHECK(std::strcmp(reg::presetKey(p), k) == 0);
        reg::loadPreset(p);
    }
    CHECK(reg::presetRejectCount() == 0);
    reg::Preset p;
    CHECK(!reg::presetFromKey("bogus", &p));
    CHECK(!reg::presetFromKey(nullptr, &p));
}

void test_heating_preset_decodes() {
    fresh();
    reg::loadPreset(reg::Preset::HEATING);
    CHECK_FIELD("compressor_freq", 50);
    CHECK_FIELD("fan_speed", 700);
    CHECK_FIELD("outlet_water_temp", 42);
    CHECK_FIELD("inlet_water_temp", 35);
    CHECK_FIELD("outdoor_ambient_temp", 5);
    CHECK_FIELD("pump_on", 1);
    CHECK_FIELD("fan_on", 1);
    CHECK_FIELD("realtime_power", 2800);
    CHECK_FIELD("working_mode", (int32_t)MaconWorkingMode::FloorHeating);
    CHECK_FIELD("operating_direction", (int32_t)MaconMode::Heating);
    MaconState s{};
    tuya_state::decode(&s);
    CHECK(std::strcmp(operation_name(decode_operation(s)), "Heating") == 0);
}

void test_cooling_preset_decodes() {
    fresh();
    reg::loadPreset(reg::Preset::COOLING);
    CHECK_FIELD("compressor_freq", 60);
    CHECK_FIELD("cooling_on", 1);
    CHECK_FIELD("outlet_water_temp", 8);
    CHECK_FIELD("working_mode", (int32_t)MaconWorkingMode::Cooling);
    CHECK_FIELD("operating_direction", (int32_t)MaconMode::Cooling);
}

void test_defrost_preset_negative_temps() {
    fresh();
    reg::loadPreset(reg::Preset::DEFROST);
    CHECK_FIELD("defrost_on", 1);
    CHECK_FIELD("outdoor_ambient_temp", -2);
    CHECK_FIELD("outdoor_coil_temp", -5);
}

void test_preset_does_not_leak_previous_state() {
    fresh();
    reg::loadPreset(reg::Preset::COOLING);
    reg::loadPreset(reg::Preset::HOT_WATER);
    CHECK_FIELD("cooling_on", 0);
    CHECK_FIELD("fan_speed", 0);
    CHECK_FIELD("working_mode", (int32_t)MaconWorkingMode::HotWater);
    CHECK_FIELD("operating_direction", (int32_t)MaconMode::Heating);
    reg::loadPreset(reg::Preset::IDLE);
    CHECK_FIELD("compressor_freq", 0);
    CHECK_FIELD("pump_on", 0);
}

void test_fault_preset_and_clear() {
    fresh();
    reg::loadPreset(reg::Preset::FAULT_P01);
    MaconFault f[8];
    const size_t n = activeFaults(f, 8);
    CHECK(n >= 1);
    bool saw = false;
    for (size_t i = 0; i < n; ++i) saw |= std::strcmp(f[i].code, "P01") == 0;
    CHECK(saw);

    reg::clearErrors();
    CHECK(activeFaults(f, 8) == 0);

    // A new preset also wipes faults.
    reg::loadPreset(reg::Preset::FAULT_P01);
    reg::loadPreset(reg::Preset::HEATING);
    CHECK(activeFaults(f, 8) == 0);
}

void test_every_catalog_fault_round_trips() {
    // Every fault site the library knows can be lit and decodes back to the
    // same code -- the property the RS485 suite relies on end to end.
    for (size_t i = 0; i < MACON_FAULT_BITS_COUNT; ++i) {
        const MaconFaultBit &b = MACON_FAULT_BITS[i];
        if (b.id == MaconFaultId::Unknown) continue;  // RUN indicator
        fresh();
        {
            tuya_state::Access a;
            a.image().set_fault(b.id, true);
        }
        MaconFault f[16];
        const size_t n = activeFaults(f, 16);
        bool saw = false;
        for (size_t k = 0; k < n; ++k) saw |= std::strcmp(f[k].code, b.code) == 0;
        if (!saw) std::fprintf(stderr, "fault %s did not round-trip\n", b.code);
        CHECK(saw);
    }
}

void test_controller_write_updates_semantic_state() {
    // Golden fc=0x06 frames the controller sends (from the 2026-05 captures):
    //   cooling setpoint 24 -> 55aaf006 0000 0001 18
    //   hot-water setpoint 38 -> 55aaf006 0002 0001 26
    fresh();
    const uint8_t cool[] = {0x18};
    CHECK(tuya_state::applyWrite(0x0000, cool, sizeof(cool)));
    CHECK_FIELD("cooling_setpoint", 24);

    const uint8_t hw[] = {0x26};
    CHECK(tuya_state::applyWrite(0x0002, hw, sizeof(hw)));
    CHECK_FIELD("hot_water_setpoint", 38);

    // Writes outside every window are rejected and change nothing.
    const uint32_t gen = tuya_state::generation();
    const uint8_t junk[] = {0x55};
    CHECK(!tuya_state::applyWrite(0x7FFF, junk, sizeof(junk)));
    CHECK(tuya_state::generation() == gen);
    CHECK_FIELD("hot_water_setpoint", 38);
}

void test_access_batches_and_bumps_generation() {
    fresh();
    const uint32_t g0 = tuya_state::generation();
    {
        tuya_state::Access a;
        MaconImage &img = a.image();
        CHECK(macon_field_set(img, *macon_field_find("outlet_water_temp"), 33) == MaconSetResult::Ok);
        CHECK(macon_field_set(img, *macon_field_find("compressor_freq"), 45) == MaconSetResult::Ok);
    }
    CHECK(tuya_state::generation() != g0);
    CHECK_FIELD("outlet_water_temp", 33);
    CHECK_FIELD("compressor_freq", 45);

    // Staging on a copy (the PATCH /api/state pattern) leaves the live image
    // untouched until committed.
    MaconImage staged;
    {
        tuya_state::Access a;
        staged = a.image();
    }
    CHECK(macon_field_set(staged, *macon_field_find("outlet_water_temp"), 50) == MaconSetResult::Ok);
    CHECK_FIELD("outlet_water_temp", 33);
    {
        tuya_state::Access a;
        a.image() = staged;
    }
    CHECK_FIELD("outlet_water_temp", 50);
}

void test_served_windows_carry_oem_baseline() {
    // Bytes the library doesn't name keep the real unit's captured values, so
    // a controller reading the full window sees an authentic payload.
    fresh();
    uint8_t buf[tuya_state::MAX_WINDOW_BYTES];
    for (size_t w = 0; w < tuya_codec::KNOWN_WINDOWS_COUNT; ++w) {
        const auto &win = tuya_codec::KNOWN_WINDOWS[w];
        CHECK(tuya_state::snapshot(win.field_a, win.field_b, buf, sizeof(buf)));
    }
}

}  // namespace

int main() {
    test_init_is_idle_with_no_faults();
    test_every_preset_applies_cleanly();
    test_heating_preset_decodes();
    test_cooling_preset_decodes();
    test_defrost_preset_negative_temps();
    test_preset_does_not_leak_previous_state();
    test_fault_preset_and_clear();
    test_every_catalog_fault_round_trips();
    test_controller_write_updates_semantic_state();
    test_access_batches_and_bumps_generation();
    test_served_windows_carry_oem_baseline();

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_presets_semantic: all passed\n");
    return 0;
}
