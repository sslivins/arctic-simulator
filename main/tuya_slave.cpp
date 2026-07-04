// ---------------------------------------------------------------------------
// tuya_slave — implementation.
//
// Split into a host-portable core (frame parsing, snapshot, encode, stats,
// test seam) and an ESP-only UART task. Everything inside
// `#if defined(ESP_PLATFORM)` is skipped on the host so the unit tests can
// link this .cpp directly.
// ---------------------------------------------------------------------------

#include "tuya_slave.h"
#include "tuya_codec/tuya_codec.h"
#include "tuya_state.h"

#include <atomic>
#include <cstring>

#if defined(ESP_PLATFORM)
  #include "esp_log.h"
  #include "esp_timer.h"
  #include "driver/uart.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/queue.h"
#endif

namespace tuya_slave {

namespace {

#if defined(ESP_PLATFORM)
constexpr const char *TAG = "tuya_slv";

// Match the sniffer's empirically-derived bus settings. The real Arctic
// bus runs at 4800 baud, 8-O-1 (Odd parity gave the cleanest decode in
// scope captures). Hard-coded for now; runtime tunables can come later.
constexpr int               BAUD_RATE = 4800;
constexpr uart_parity_t     PARITY    = UART_PARITY_ODD;
constexpr uart_stop_bits_t  STOP      = UART_STOP_BITS_1;

constexpr size_t MAX_BLOB        = 512;
constexpr int    UART_BUF_SZ     = 512;
constexpr int    UART_QUEUE_SZ   = 20;
constexpr int    RX_TOUT_THRESH  = 10;   // bit-times of silence
constexpr int    TX_TIMEOUT_MS   = 100;
constexpr int64_t STATS_INTERVAL_MS = 10000;

QueueHandle_t s_uart_queue = nullptr;
TaskHandle_t  s_task        = nullptr;
bool          s_initialized = false;
uint8_t       s_blob[MAX_BLOB];
size_t        s_blob_len = 0;
#endif  // ESP_PLATFORM

std::atomic<uint32_t> s_frames_seen{0};
std::atomic<uint32_t> s_requests_handled{0};
std::atomic<uint32_t> s_responses_sent{0};
std::atomic<uint32_t> s_parse_errors{0};
std::atomic<uint32_t> s_unknown_windows{0};
std::atomic<uint32_t> s_snapshot_failures{0};
std::atomic<uint32_t> s_tx_truncated{0};
std::atomic<uint32_t> s_uart_errors{0};

// Raw RX capture (debug aid; see header). The buffer and length are only
// touched by the slave task (writer) and the API handler (reader). The reader
// disarms first, so once s_cap_armed is false the buffer is stable to read.
std::atomic<bool> s_cap_armed{false};
uint8_t           s_cap_buf[CAPTURE_BUF_SZ];
std::atomic<size_t> s_cap_len{0};

// Decoded fc=0x06 command log (debug aid): ring of the most recent commands
// the controller sent, so button presses / setpoint changes can be mapped
// without decoding raw hex. Written by the slave task, read by the API.
std::atomic<uint32_t> s_commands_seen{0};
CommandRec s_cmd_ring[COMMAND_RING_SZ];
std::atomic<uint32_t> s_cmd_count{0};   // total ever recorded (monotonic)

// ----------------------------------------------------------------------
// Pure handler — encodes a response into `out` for a parsed request frame.
// Returns true if `out` now holds *out_len bytes of response. Returns
// false (and leaves *out_len = 0) if the request couldn't be served
// (unknown window, snapshot miss, buffer too small). Counters are
// updated in either case.
// ----------------------------------------------------------------------
bool encodeResponseFor(const tuya_codec::ParsedFrame &pf,
                       uint8_t *out, size_t out_capacity, size_t *out_len) {
    *out_len = 0;
    const tuya_codec::RegWindow *win = pf.window;
    if (!win) {
        s_unknown_windows.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t payload[tuya_state::MAX_WINDOW_BYTES];
    if (!tuya_state::snapshot(win->field_a, win->field_b,
                              payload, sizeof(payload))) {
        s_snapshot_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    size_t enc = tuya_codec::encode_response(out, out_capacity,
                                             pf.fc, win->field_a, win->field_b,
                                             payload);
    if (enc == 0) {
        s_tx_truncated.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    *out_len = enc;
    s_requests_handled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace

// =======================================================================
// Test seam (host-safe)
// =======================================================================

// Returns the number of bytes consumed from `in`. Writes a response into
// `out` (length in *out_len) if the first complete frame was a request we
// could handle. Otherwise *out_len is 0.
size_t handleBytesForTest(const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t out_capacity,
                          size_t *out_len) {
    *out_len = 0;
    if (in_len < tuya_codec::MIN_FRAME_LEN) return 0;

    size_t start = tuya_codec::find_frame_start(in, in_len);
    if (start == in_len) return in_len;   // discard junk, no frame
    if (start > 0) return start;          // caller advances past junk

    if (in_len < tuya_codec::HDR_LEN) return 0;
    const uint8_t  dir     = in[2];
    const uint8_t  fc      = in[3];
    const uint16_t field_a = (uint16_t)((in[4] << 8) | in[5]);
    const uint16_t field_b = (uint16_t)((in[6] << 8) | in[7]);
    size_t need = (fc == tuya_codec::FC_CMD)
                    ? (tuya_codec::HDR_LEN + tuya_codec::CHK_LEN)   // command: fixed 9
                    : tuya_codec::frame_total_len(dir, field_b);
    if (need == 0) return 2;              // bogus header — skip past 55 AA
    if (in_len < need) return 0;          // wait for more bytes

    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(in, need, pf);
    if (pr != tuya_codec::ParseResult::OK) {
        s_parse_errors.fetch_add(1, std::memory_order_relaxed);
        return need;
    }
    s_frames_seen.fetch_add(1, std::memory_order_relaxed);

    if (pf.dir == tuya_codec::DIR_REQUEST) {
        if (pf.fc == tuya_codec::FC_CMD) {
            // Controller command (e.g. power on/off, mode, setpoint). Record it
            // for reverse-engineering and ACK it so the controller sees the
            // command as accepted.
            s_commands_seen.fetch_add(1, std::memory_order_relaxed);
            uint32_t idx = s_cmd_count.fetch_add(1, std::memory_order_relaxed);
            s_cmd_ring[idx % COMMAND_RING_SZ] = { field_a, field_b };
            size_t enc = tuya_codec::encode_command_ack(out, out_capacity,
                                                        field_a, field_b);
            if (enc > 0) {
                *out_len = enc;
                s_responses_sent.fetch_add(1, std::memory_order_relaxed);
            } else {
                s_tx_truncated.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (encodeResponseFor(pf, out, out_capacity, out_len)) {
            s_responses_sent.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // DIR_RESPONSE: ignore (most likely our own TX echo).
    return need;
}

Stats getStats() {
    Stats s = {};
    s.frames_seen        = s_frames_seen.load(std::memory_order_relaxed);
    s.requests_handled   = s_requests_handled.load(std::memory_order_relaxed);
    s.responses_sent     = s_responses_sent.load(std::memory_order_relaxed);
    s.parse_errors       = s_parse_errors.load(std::memory_order_relaxed);
    s.unknown_windows    = s_unknown_windows.load(std::memory_order_relaxed);
    s.snapshot_failures  = s_snapshot_failures.load(std::memory_order_relaxed);
    s.tx_truncated       = s_tx_truncated.load(std::memory_order_relaxed);
    s.uart_errors        = s_uart_errors.load(std::memory_order_relaxed);
    s.commands_seen      = s_commands_seen.load(std::memory_order_relaxed);
    return s;
}

void resetStats() {
    s_frames_seen.store(0);
    s_requests_handled.store(0);
    s_responses_sent.store(0);
    s_parse_errors.store(0);
    s_unknown_windows.store(0);
    s_snapshot_failures.store(0);
    s_tx_truncated.store(0);
    s_uart_errors.store(0);
    s_commands_seen.store(0);
}

size_t getRecentCommands(CommandRec *out, size_t max, uint32_t *total) {
    uint32_t count = s_cmd_count.load(std::memory_order_relaxed);
    if (total) *total = count;
    if (!out || max == 0 || count == 0) return 0;
    uint32_t avail = (count < COMMAND_RING_SZ) ? count : COMMAND_RING_SZ;
    uint32_t n = (avail < max) ? avail : (uint32_t)max;
    // Return the n most recent, oldest-first.
    uint32_t first = count - n;   // index of first record to return
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = s_cmd_ring[(first + i) % COMMAND_RING_SZ];
    }
    return n;
}

// ----------------------------------------------------------------------
// Raw RX capture (debug aid). Writer is the slave task's captureAppend();
// reader is captureCopy(), which disarms first so the buffer is stable.
// ----------------------------------------------------------------------
void captureArm() {
    s_cap_armed.store(false, std::memory_order_release);
    s_cap_len.store(0, std::memory_order_relaxed);
    s_cap_armed.store(true, std::memory_order_release);
}

bool captureArmed() { return s_cap_armed.load(std::memory_order_acquire); }

size_t captureLen() { return s_cap_len.load(std::memory_order_relaxed); }

size_t captureCopy(uint8_t *out, size_t cap) {
    s_cap_armed.store(false, std::memory_order_release);
    size_t n = s_cap_len.load(std::memory_order_relaxed);
    if (n > cap) n = cap;
    if (out && n) std::memcpy(out, s_cap_buf, n);
    return n;
}

namespace {
// Append raw RX bytes while armed; auto-disarm when the buffer fills.
[[maybe_unused]] inline void captureAppend(const uint8_t *data, size_t len) {
    if (!s_cap_armed.load(std::memory_order_acquire)) return;
    size_t have = s_cap_len.load(std::memory_order_relaxed);
    size_t room = (have < CAPTURE_BUF_SZ) ? (CAPTURE_BUF_SZ - have) : 0;
    size_t n = (len < room) ? len : room;
    if (n) {
        std::memcpy(s_cap_buf + have, data, n);
        s_cap_len.store(have + n, std::memory_order_relaxed);
    }
    if (n < len) s_cap_armed.store(false, std::memory_order_release);  // full
}
}  // namespace

// =======================================================================
// ESP-only runtime: UART task + init/deinit
// =======================================================================

#if defined(ESP_PLATFORM)

namespace {

void consumeFront(size_t n) {
    if (n >= s_blob_len) s_blob_len = 0;
    else { std::memmove(s_blob, s_blob + n, s_blob_len - n); s_blob_len -= n; }
}

void txResponse(const uint8_t *frame, size_t len) {
    const uart_port_t port = (uart_port_t)CONFIG_SIMULATOR_UART_PORT;
    int wrote = uart_write_bytes(port, reinterpret_cast<const char *>(frame), len);
    if (wrote < 0 || static_cast<size_t>(wrote) != len) {
        s_tx_truncated.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Block until the line drains so the next request doesn't collide with
    // our outbound bytes. Important on auto-direction RS-485 transceivers
    // (DE de-asserts when TX FIFO empties).
    uart_wait_tx_done(port, pdMS_TO_TICKS(TX_TIMEOUT_MS));
}

// Process as many complete frames as are present in s_blob[].
void tryExtractFrames() {
    int iters = 0;
    while (s_blob_len >= tuya_codec::MIN_FRAME_LEN && iters++ < 32) {
        uint8_t  out_buf[tuya_codec::MAX_FRAME_LEN];
        size_t   out_len = 0;
        size_t   consumed = handleBytesForTest(s_blob, s_blob_len,
                                               out_buf, sizeof(out_buf),
                                               &out_len);
        if (consumed == 0) return;        // wait for more bytes
        if (out_len > 0) txResponse(out_buf, out_len);
        consumeFront(consumed);
    }
}

void slaveTask(void * /*arg*/) {
    const uart_port_t port = (uart_port_t)CONFIG_SIMULATOR_UART_PORT;
    uart_event_t event;
    int64_t last_stats_ms = 0;

    while (true) {
        if (xQueueReceive(s_uart_queue, &event, pdMS_TO_TICKS(10))) {
            switch (event.type) {
                case UART_DATA: {
                    size_t avail = 0;
                    uart_get_buffered_data_len(port, &avail);
                    while (avail > 0 && s_blob_len < MAX_BLOB) {
                        size_t room    = MAX_BLOB - s_blob_len;
                        size_t to_read = (avail < room) ? avail : room;
                        int got = uart_read_bytes(port, s_blob + s_blob_len,
                                                  to_read, pdMS_TO_TICKS(5));
                        if (got > 0) { captureAppend(s_blob + s_blob_len, (size_t)got); s_blob_len += (size_t)got; }
                        else break;
                        uart_get_buffered_data_len(port, &avail);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    s_uart_errors.fetch_add(1, std::memory_order_relaxed);
                    uart_flush_input(port);
                    xQueueReset(s_uart_queue);
                    s_blob_len = 0;
                    break;
                case UART_PARITY_ERR:
                case UART_FRAME_ERR:
                    s_uart_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                default: break;
            }
        }

        tryExtractFrames();

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - last_stats_ms) >= STATS_INTERVAL_MS) {
            ESP_LOGI(TAG,
                "Stats: frames=%lu req=%lu resp=%lu parse_err=%lu unk=%lu snap_fail=%lu tx_trunc=%lu uart_err=%lu",
                (unsigned long)s_frames_seen.load(),
                (unsigned long)s_requests_handled.load(),
                (unsigned long)s_responses_sent.load(),
                (unsigned long)s_parse_errors.load(),
                (unsigned long)s_unknown_windows.load(),
                (unsigned long)s_snapshot_failures.load(),
                (unsigned long)s_tx_truncated.load(),
                (unsigned long)s_uart_errors.load());
            last_stats_ms = now_ms;
        }
    }
}

}  // namespace

esp_err_t init() {
    if (s_initialized) return ESP_OK;
    tuya_state::init();

    const uart_port_t port = (uart_port_t)CONFIG_SIMULATOR_UART_PORT;
    ESP_LOGI(TAG, "Init Tuya slave UART%d (%d baud 8-O-1, RX=%d TX=%d)",
             port, BAUD_RATE,
             CONFIG_SIMULATOR_RS485_RX_PIN, CONFIG_SIMULATOR_RS485_TX_PIN);

    uart_config_t cfg = {};
    cfg.baud_rate  = BAUD_RATE;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = PARITY;
    cfg.stop_bits  = STOP;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(port, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "param_config: %s", esp_err_to_name(err)); return err; }

    err = uart_set_pin(port,
                       CONFIG_SIMULATOR_RS485_TX_PIN,
                       CONFIG_SIMULATOR_RS485_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_pin: %s", esp_err_to_name(err)); return err; }

    err = uart_driver_install(port, UART_BUF_SZ * 2, UART_BUF_SZ,
                              UART_QUEUE_SZ, &s_uart_queue, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(err)); return err; }

    err = uart_set_rx_timeout(port, RX_TOUT_THRESH);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_rx_timeout: %s", esp_err_to_name(err));

    BaseType_t ok = xTaskCreatePinnedToCore(slaveTask, "tuya_slv",
                                            8192, nullptr, 10, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn slave task");
        uart_driver_delete(port);
        s_uart_queue = nullptr;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Tuya slave task started");
    return ESP_OK;
}

void deinit() {
    if (!s_initialized) return;
    if (s_task) { vTaskDelete(s_task); s_task = nullptr; }
    const uart_port_t port = (uart_port_t)CONFIG_SIMULATOR_UART_PORT;
    uart_driver_delete(port);
    s_uart_queue = nullptr;
    s_blob_len = 0;
    s_initialized = false;
}

bool isInitialized() { return s_initialized; }

#else  // !ESP_PLATFORM (host build for unit tests)

esp_err_t init() { return 0; }
void      deinit() {}
bool      isInitialized() { return false; }

#endif

}  // namespace tuya_slave
