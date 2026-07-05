// ---------------------------------------------------------------------------
// tuya_state — implementation
//
// Builds on both target (ESP-IDF / FreeRTOS) and host (native tests). The
// mutex picks the platform-appropriate primitive via a small Lock helper
// at the top of this file.
// ---------------------------------------------------------------------------

#include "tuya_state.h"
#include "tuya_codec.h"

#include <cstring>

#if defined(ESP_PLATFORM)
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#else
  #include <mutex>
#endif

namespace tuya_state {

namespace {

// ---------------- Platform-specific mutex --------------------------------

#if defined(ESP_PLATFORM)
class Mutex {
public:
    Mutex()  { handle_ = xSemaphoreCreateMutex(); }
    ~Mutex() { if (handle_) vSemaphoreDelete(handle_); }
    void lock()   { if (handle_) xSemaphoreTake(handle_, portMAX_DELAY); }
    void unlock() { if (handle_) xSemaphoreGive(handle_); }
private:
    SemaphoreHandle_t handle_ = nullptr;
};
#else
class Mutex {
public:
    void lock()   { m_.lock();   }
    void unlock() { m_.unlock(); }
private:
    std::mutex m_;
};
#endif

class Guard {
public:
    explicit Guard(Mutex &m) : m_(m) { m_.lock(); }
    ~Guard()                          { m_.unlock(); }
    Guard(const Guard &)            = delete;
    Guard &operator=(const Guard &) = delete;
private:
    Mutex &m_;
};

// ---------------- Storage ------------------------------------------------

struct Slot {
    uint16_t field_a   = 0;
    uint16_t field_b   = 0;
    uint16_t reg_base  = 0;
    uint8_t  prefix    = 0;
    uint8_t  bytes[MAX_WINDOW_BYTES] = {};
    bool     in_use    = false;
};

Slot   s_slots[MAX_WINDOWS];
size_t s_slot_count = 0;
bool   s_initialized = false;

// Static singleton — function-local-static guarantees first-call init order
// across translation units without resorting to a global ctor.
Mutex &mutex() {
    static Mutex m;
    return m;
}

// Find the slot index for a window. Caller must hold the mutex.
int findSlotLocked(uint16_t field_a, uint16_t field_b) {
    for (size_t i = 0; i < s_slot_count; ++i) {
        if (s_slots[i].in_use &&
            s_slots[i].field_a == field_a &&
            s_slots[i].field_b == field_b) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Resolve reg_addr -> (slot index, byte offset) by walking the registered
// windows. Returns false if no window contains this register.
// Caller must hold the mutex.
bool resolveRegLocked(uint16_t reg_addr, int &slot_out, size_t &offset_out) {
    for (size_t i = 0; i < s_slot_count; ++i) {
        const Slot &s = s_slots[i];
        if (!s.in_use) continue;
        // Data-region length = field_b - prefix
        if (s.field_b <= s.prefix) continue;
        const uint16_t data_len = static_cast<uint16_t>(s.field_b - s.prefix);
        if (reg_addr >= s.reg_base && reg_addr < s.reg_base + data_len) {
            slot_out   = static_cast<int>(i);
            offset_out = static_cast<size_t>(s.prefix + (reg_addr - s.reg_base));
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------- Lifecycle ----------------------------------------------

void init() {
    Guard g(mutex());
    if (s_initialized) return;

    s_slot_count = 0;
    const size_t n = tuya_codec::KNOWN_WINDOWS_COUNT;
    for (size_t i = 0; i < n && s_slot_count < MAX_WINDOWS; ++i) {
        const auto &w = tuya_codec::KNOWN_WINDOWS[i];
        if (w.field_b > MAX_WINDOW_BYTES) continue;  // skip pathologically large
        Slot &slot = s_slots[s_slot_count++];
        slot.field_a  = w.field_a;
        slot.field_b  = w.field_b;
        slot.reg_base = w.reg_base;
        slot.prefix   = w.prefix_len;
        std::memset(slot.bytes, 0, sizeof(slot.bytes));
        slot.in_use   = true;
    }
    s_initialized = true;
}

void resetForTest() {
    Guard g(mutex());
    for (size_t i = 0; i < MAX_WINDOWS; ++i) {
        s_slots[i] = Slot{};
    }
    s_slot_count  = 0;
    s_initialized = false;
}

bool isInitialized() {
    Guard g(mutex());
    return s_initialized;
}

size_t windowCount() {
    Guard g(mutex());
    return s_slot_count;
}

// ---------------- Read ---------------------------------------------------

bool snapshot(uint16_t field_a, uint16_t field_b,
              uint8_t *out_buf, size_t out_buf_capacity) {
    if (!out_buf || out_buf_capacity < field_b) return false;
    Guard g(mutex());
    const int idx = findSlotLocked(field_a, field_b);
    if (idx < 0) return false;
    std::memcpy(out_buf, s_slots[idx].bytes, field_b);
    return true;
}

uint8_t getByte(uint16_t field_a, size_t offset) {
    Guard g(mutex());
    for (size_t i = 0; i < s_slot_count; ++i) {
        const Slot &s = s_slots[i];
        if (!s.in_use || s.field_a != field_a) continue;
        if (offset >= s.field_b) return 0;
        return s.bytes[offset];
    }
    return 0;
}

// ---------------- Write --------------------------------------------------

bool writeWindow(uint16_t field_a, uint16_t field_b,
                 const uint8_t *payload, size_t payload_len) {
    if (!payload) return false;
    if (payload_len != field_b) return false;
    Guard g(mutex());
    const int idx = findSlotLocked(field_a, field_b);
    if (idx < 0) return false;
    std::memcpy(s_slots[idx].bytes, payload, field_b);
    return true;
}

bool setByte(uint16_t field_a, size_t offset, uint8_t value) {
    Guard g(mutex());
    for (size_t i = 0; i < s_slot_count; ++i) {
        Slot &s = s_slots[i];
        if (!s.in_use || s.field_a != field_a) continue;
        if (offset >= s.field_b) return false;
        s.bytes[offset] = value;
        return true;
    }
    return false;
}

// ---------------- Projection ---------------------------------------------

uint16_t projectGet(uint16_t reg_addr) {
    Guard g(mutex());
    int    slot_idx = -1;
    size_t offset   = 0;
    if (!resolveRegLocked(reg_addr, slot_idx, offset)) return 0;
    return static_cast<uint16_t>(s_slots[slot_idx].bytes[offset]);
}

bool projectSet(uint16_t reg_addr, uint16_t value) {
    Guard g(mutex());
    int    slot_idx = -1;
    size_t offset   = 0;
    if (!resolveRegLocked(reg_addr, slot_idx, offset)) return false;
    s_slots[slot_idx].bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    return true;
}

bool projectKnows(uint16_t reg_addr) {
    Guard g(mutex());
    int    slot_idx = -1;
    size_t offset   = 0;
    return resolveRegLocked(reg_addr, slot_idx, offset);
}

}  // namespace tuya_state
