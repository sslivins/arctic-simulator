// ---------------------------------------------------------------------------
// tuya_state — implementation (one mutex-guarded arctic::MaconImage).
//
// Builds on both target (ESP-IDF / FreeRTOS) and host (native tests).
// ---------------------------------------------------------------------------

#include "tuya_state.h"
#include "tuya_codec.h"

#include <atomic>

#if defined(ESP_PLATFORM)
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#else
  #include <mutex>
#endif

namespace tuya_state {

namespace {

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

Mutex &mutex() {
    static Mutex m;
    return m;
}

arctic::MaconImage   s_image;
bool                 s_initialized = false;
std::atomic<uint32_t> s_generation{0};

void bump() { s_generation.fetch_add(1, std::memory_order_relaxed); }

const tuya_codec::RegWindow *windowByA(uint16_t field_a) {
    for (size_t i = 0; i < tuya_codec::KNOWN_WINDOWS_COUNT; ++i) {
        if (tuya_codec::KNOWN_WINDOWS[i].field_a == field_a) return &tuya_codec::KNOWN_WINDOWS[i];
    }
    return nullptr;
}

// Register served at payload `offset` of `win`, or 0 for a prefix byte.
uint16_t regAt(const tuya_codec::RegWindow &win, size_t offset) {
    if (offset < win.prefix_len) return 0;
    return static_cast<uint16_t>(win.reg_base + (offset - win.prefix_len));
}

bool servedLocked(uint16_t reg) {
    for (size_t i = 0; i < tuya_codec::KNOWN_WINDOWS_COUNT; ++i) {
        const auto &w = tuya_codec::KNOWN_WINDOWS[i];
        const uint16_t n = static_cast<uint16_t>(w.field_b - w.prefix_len);
        if (reg >= w.reg_base && reg < w.reg_base + n) return true;
    }
    return false;
}

}  // namespace

// ---------------- Lifecycle ----------------------------------------------

void init() {
    Guard g(mutex());
    if (s_initialized) return;
    s_image.clear();
    s_image.fill_baseline();
    s_initialized = true;
    bump();
}

void resetForTest() {
    Guard g(mutex());
    s_image.clear();
    s_initialized = false;
    bump();
}

bool isInitialized() {
    Guard g(mutex());
    return s_initialized;
}

size_t windowCount() {
    Guard g(mutex());
    return s_initialized ? tuya_codec::KNOWN_WINDOWS_COUNT : 0;
}

// ---------------- Wire ---------------------------------------------------

bool snapshot(uint16_t field_a, uint16_t field_b,
              uint8_t *out_buf, size_t out_buf_capacity) {
    Guard g(mutex());
    if (!s_initialized) return false;
    return s_image.read_window(field_a, field_b, out_buf, out_buf_capacity) == field_b &&
           field_b > 0;
}

uint8_t getByte(uint16_t field_a, size_t offset) {
    Guard g(mutex());
    const tuya_codec::RegWindow *w = windowByA(field_a);
    if (!s_initialized || !w || offset >= w->field_b) return 0;
    uint16_t v = 0;
    const uint16_t reg = regAt(*w, offset);
    return (reg && s_image.get_register(reg, &v)) ? static_cast<uint8_t>(v) : 0;
}

bool writeWindow(uint16_t field_a, uint16_t field_b,
                 const uint8_t *payload, size_t payload_len) {
    if (!payload || payload_len != field_b) return false;
    Guard g(mutex());
    const tuya_codec::RegWindow *w = tuya_codec::find_window(field_a, field_b);
    if (!s_initialized || !w) return false;
    s_image.ingest_bytes(w->reg_base, payload + w->prefix_len, field_b - w->prefix_len);
    bump();
    return true;
}

bool setByte(uint16_t field_a, size_t offset, uint8_t value) {
    Guard g(mutex());
    const tuya_codec::RegWindow *w = windowByA(field_a);
    if (!s_initialized || !w || offset >= w->field_b) return false;
    const uint16_t reg = regAt(*w, offset);
    if (reg == 0) return true;   // static prefix byte: nothing to store
    const bool ok = s_image.set_register(reg, value);
    if (ok) bump();
    return ok;
}

bool applyWrite(uint16_t wire_addr, const uint8_t *data, size_t len) {
    Guard g(mutex());
    if (!s_initialized) return false;
    const bool ok = s_image.apply_write(wire_addr, data, len);
    if (ok) bump();
    return ok;
}

// ---------------- Raw projection -----------------------------------------

uint16_t projectGet(uint16_t reg_addr) {
    Guard g(mutex());
    uint16_t v = 0;
    if (!servedLocked(reg_addr) || !s_image.get_register(reg_addr, &v)) return 0;
    return v;
}

bool projectSet(uint16_t reg_addr, uint16_t value) {
    Guard g(mutex());
    if (!servedLocked(reg_addr)) return false;
    const bool ok = s_image.set_register(reg_addr, static_cast<uint16_t>(value & 0xFF));
    if (ok) bump();
    return ok;
}

bool projectKnows(uint16_t reg_addr) {
    Guard g(mutex());
    return servedLocked(reg_addr);
}

// ---------------- Semantic -----------------------------------------------

Access::Access()  { mutex().lock(); }
Access::~Access() { bump(); mutex().unlock(); }
arctic::MaconImage &Access::image() { return s_image; }

void decode(arctic::MaconState *out) {
    Guard g(mutex());
    s_image.decode(out);
}

uint32_t generation() { return s_generation.load(std::memory_order_relaxed); }

}  // namespace tuya_state
