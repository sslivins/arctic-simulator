/*
 * Simulator presets + raw register debug access — implementation.
 *
 * Presets are lists of NAMED fields applied through the arctic-macon library
 * (macon_field_set), over a baseline that is the real OEM mainboard's wire
 * image. No register numbers live here.
 */
#include "register_map.h"
#include "tuya_state.h"
#include "tuya_codec.h"
#include "macon_fields.h"
#if defined(ESP_PLATFORM)
  #include "esp_log.h"
#else
  #define ESP_LOGI(tag, ...) ((void)(tag))
  #define ESP_LOGE(tag, ...) ((void)(tag))
#endif
#include <string.h>

using namespace arctic;

static const char* TAG = "reg";

namespace reg {

namespace {

// OEM baseline: payloads served by a real Macon mainboard (idle, hot-water
// mode), verbatim from tests/data/capture_raw.jsonl (2026-05-03). Seeding
// from these keeps every byte the library doesn't name (installer Cn
// parameters, unmapped telemetry) identical to the real unit.
const uint8_t kOemHolding[58] = {
    0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x20,0x14,0x23,0x2d,0x2d,0x32,0xfa,0x03,0x00,
    0x00,0x00,0x00,0x00,0x04,0x2d,0x00,0x1c,0x12,0x05,0x05,0x00,0x00,0xf6,0x3c,0xf9,
    0x0a,0x2d,0x05,0x0c,0x5f,0x0f,0xe2,0x32,0xff,0x0a,0x0a,0x05,0x02,0x05,0xff,0x00,
    0x00,0x00,0x00,0x00,0x14,0xf1,0x00,0x00,0x00,0x00,
};
const uint8_t kOemTelemetry[50] = {
    0x0a,0x28,0x32,0x05,0x01,0x00,0x0f,0x1e,0x17,0x06,0x09,0x11,0x23,0x00,0x20,0x23,
    0x1e,0x28,0x00,0x00,0x0c,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x0a,0x0f,0x0b,0x0b,0x0f,0x0e,0x00,
    0x13,0x00,
};

struct FieldValue { const char *name; int32_t value; };

int s_rejects = 0;

constexpr int32_t WM_COOLING   = static_cast<int32_t>(MaconWorkingMode::Cooling);
constexpr int32_t WM_FLOOR     = static_cast<int32_t>(MaconWorkingMode::FloorHeating);
constexpr int32_t WM_HOT_WATER = static_cast<int32_t>(MaconWorkingMode::HotWater);
constexpr int32_t DIR_HEATING  = static_cast<int32_t>(MaconMode::Heating);
constexpr int32_t DIR_COOLING  = static_cast<int32_t>(MaconMode::Cooling);

// Every preset starts from this fully-specified "idle, powered, no faults" state
// (unit enabled, matching the OEM idle capture where reg2007 = 0x20)
// so no value leaks from the previous preset or a controller write.
const FieldValue kCommon[] = {
    {"unit_on", 1}, {"fan_on", 0}, {"pump_on", 0}, {"cooling_on", 0},
    {"defrost_on", 0}, {"compressor_icon", 0},
    {"compressor_freq", 0}, {"fan_speed", 0}, {"realtime_power", 0},
    {"ac_voltage", 230}, {"ac_current", 0}, {"dc_voltage", 320}, {"primary_eev", 17},
    {"outdoor_ambient_temp", 20}, {"inlet_water_temp", 25}, {"outlet_water_temp", 25},
    {"water_tank_temp", 45}, {"discharge_temp", 25}, {"suction_temp", 20},
    {"outdoor_coil_temp", 20}, {"indoor_coil_temp", 20}, {"ipm_temp", 20},
    {"cooling_setpoint", 10}, {"heating_setpoint", 40}, {"hot_water_setpoint", 50},
    {"hot_water_ceiling", 55},
    {"working_mode", WM_HOT_WATER}, {"operating_direction", DIR_HEATING},
};

const FieldValue kHeating[] = {
    {"unit_on", 1}, {"compressor_icon", 1}, {"pump_on", 1}, {"fan_on", 1},
    {"compressor_freq", 50}, {"outdoor_ambient_temp", 5}, {"inlet_water_temp", 35},
    {"outlet_water_temp", 42}, {"discharge_temp", 75}, {"suction_temp", 3},
    {"outdoor_coil_temp", 2}, {"ipm_temp", 45}, {"primary_eev", 200},
    {"realtime_power", 2800}, {"ac_current", 12}, {"dc_voltage", 360}, {"fan_speed", 700},
    {"working_mode", WM_FLOOR},
};

const FieldValue kCooling[] = {
    {"unit_on", 1}, {"compressor_icon", 1}, {"pump_on", 1}, {"fan_on", 1}, {"cooling_on", 1},
    {"compressor_freq", 60}, {"outdoor_ambient_temp", 35}, {"inlet_water_temp", 12},
    {"outlet_water_temp", 8}, {"discharge_temp", 65}, {"suction_temp", 5},
    {"indoor_coil_temp", 6}, {"ipm_temp", 50}, {"primary_eev", 250},
    {"realtime_power", 3000}, {"ac_current", 13}, {"dc_voltage", 360}, {"fan_speed", 800},
    {"working_mode", WM_COOLING}, {"operating_direction", DIR_COOLING},
};

const FieldValue kHotWater[] = {
    {"unit_on", 1}, {"compressor_icon", 1}, {"pump_on", 1},
    {"compressor_freq", 55}, {"outdoor_ambient_temp", 20}, {"inlet_water_temp", 40},
    {"outlet_water_temp", 48}, {"discharge_temp", 85}, {"suction_temp", 8},
    {"ipm_temp", 48}, {"realtime_power", 3200}, {"water_tank_temp", 42},
    {"ac_current", 12}, {"dc_voltage", 360},
};

const FieldValue kDefrost[] = {
    {"unit_on", 1}, {"compressor_icon", 1}, {"pump_on", 1}, {"defrost_on", 1},
    {"compressor_freq", 40}, {"outdoor_ambient_temp", -2}, {"inlet_water_temp", 30},
    {"outlet_water_temp", 28}, {"outdoor_coil_temp", -5}, {"discharge_temp", 50},
    {"working_mode", WM_FLOOR},
};

const FieldValue kFaultP01[] = {
    {"outdoor_ambient_temp", 20},
};

struct PresetDef {
    Preset            id;
    const char       *key;
    const FieldValue *fields;
    size_t            count;
    const char       *fault_code;   // extra fault to light, or nullptr
};

#define DEF(id, key, arr, fault) { Preset::id, key, arr, sizeof(arr) / sizeof(arr[0]), fault }
const PresetDef kPresets[] = {
    { Preset::IDLE, "idle", nullptr, 0, nullptr },
    DEF(HEATING,   "heating",   kHeating,  nullptr),
    DEF(COOLING,   "cooling",   kCooling,  nullptr),
    DEF(HOT_WATER, "hot_water", kHotWater, nullptr),
    DEF(DEFROST,   "defrost",   kDefrost,  nullptr),
    DEF(FAULT_P01, "fault_p01", kFaultP01, "P01"),
};
#undef DEF

const PresetDef *findPreset(Preset p) {
    for (const PresetDef &d : kPresets) if (d.id == p) return &d;
    return nullptr;
}

void applyFields(MaconImage &img, const FieldValue *fv, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const MaconFieldDesc *d = macon_field_find(fv[i].name);
        const MaconSetResult r = d ? macon_field_set(img, *d, fv[i].value) : MaconSetResult::UnknownField;
        if (r != MaconSetResult::Ok) {
            ++s_rejects;
            // A preset the library rejects is a programming error; make it loud.
            ESP_LOGE(TAG, "preset field %s=%ld rejected (%d)", fv[i].name,
                     (long)fv[i].value, (int)r);
        }
    }
}

void seedOemBaseline(MaconImage &img) {
    const tuya_codec::RegWindow *h = tuya_codec::find_window(50, sizeof(kOemHolding));
    const tuya_codec::RegWindow *t = tuya_codec::find_window(0, sizeof(kOemTelemetry));
    if (h) img.ingest_bytes(h->reg_base, kOemHolding + h->prefix_len, sizeof(kOemHolding) - h->prefix_len);
    if (t) img.ingest_bytes(t->reg_base, kOemTelemetry + t->prefix_len, sizeof(kOemTelemetry) - t->prefix_len);
}

}  // namespace

bool presetFromKey(const char *key, Preset *out) {
    if (!key) return false;
    for (const PresetDef &d : kPresets) {
        if (strcmp(d.key, key) == 0) { if (out) *out = d.id; return true; }
    }
    return false;
}

const char *presetKey(Preset preset) {
    const PresetDef *d = findPreset(preset);
    return d ? d->key : "unknown";
}

void loadPreset(Preset preset) {
    const PresetDef *d = findPreset(preset);
    if (!d) return;
    ESP_LOGI(TAG, "Loading preset: %s", d->key);
    tuya_state::Access a;
    MaconImage &img = a.image();
    img.clear_faults();
    applyFields(img, kCommon, sizeof(kCommon) / sizeof(kCommon[0]));
    applyFields(img, d->fields, d->count);
    if (d->fault_code) img.set_fault_by_code(d->fault_code, true);
}

int presetRejectCount() { return s_rejects; }

void clearErrors() {
    tuya_state::Access a;
    a.image().clear_faults();
}

void init() {
    tuya_state::init();
    {
        tuya_state::Access a;
        seedOemBaseline(a.image());
    }
    loadPreset(Preset::IDLE);
    ESP_LOGI(TAG, "State initialised (OEM baseline + idle preset)");
}

bool isValid(uint16_t addr) { return tuya_state::projectKnows(addr); }

uint16_t get(uint16_t addr) { return tuya_state::projectGet(addr); }

esp_err_t set(uint16_t addr, uint16_t value) {
    return tuya_state::projectSet(addr, value) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

}  // namespace reg
