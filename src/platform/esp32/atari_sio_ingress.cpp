#if defined(FN_BUILD_ATARI_FUJIBUS_SIO)

#include "fujinet/platform/esp32/atari_sio_ingress.h"

#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/platform/esp32/uart_channel.h"

#include <algorithm>

extern "C" {
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace fujinet::platform::esp32 {
namespace {
constexpr std::uint32_t sio_baud = 19200;
// Default gaps (AtariSioTuning; sio.tune changes them). A real drive leaves them, as in the legacy firmware. DELAY_T5 (250 us) separates the ACK from COMPLETE
// at every speed: sent back to back, the Atari's ROM routine sometimes misses COMPLETE and waits out its
// 9 s timeout. DELAY_HSIO_DATA (1 ms from COMPLETE to the data) is for high speed only, where a driver's
// receive loop cannot take the data straight after.
constexpr std::uint8_t sio_ack = 0x41, sio_complete = 0x43, sio_error = 0x45;

config::UartConfig sio_config(config::UartConfig cfg)
{
    if (cfg.baudRate == config::UartConfig{}.baudRate) cfg.baudRate = sio_baud;
    cfg.dataBits = 8;
    cfg.parity = config::UartParity::None;
    cfg.stopBits = config::UartStopBits::One;
    cfg.flowControl = config::UartFlowControl::None;
    // TX pacing exists to stop an Amiga RS-232 host overrunning its UART. A POKEY needs none, and the
    // default 2 ms after every 16 bytes added about 30 ms to each 256-byte sector reply.
    cfg.txGapUs = 0;
    cfg.txByteGapUs = 0;
    cfg.txChunkGapUs = 0;
    return cfg;
}

void configure_sio_lines(const PinMap& pins)
{
    for (const int pin : {pins.sio.cmd, pins.sio.mtr, pins.sio.cko}) {
        if (pin >= 0 && pin < GPIO_NUM_MAX) gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT);
    }
    for (const int pin : {pins.sio.int_pin, pins.sio.proc, pins.sio.cki}) {
        if (pin >= 0 && pin < GPIO_NUM_MAX) {
            gpio_set_level(static_cast<gpio_num_t>(pin), 1);
            gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT_OD);
        }
    }
}

void IRAM_ATTR cmd_edge_isr(void* arg)
{
    static_cast<AtariSioIngress*>(arg)->note_cmd_edge();
}
}

AtariSioIngress::AtariSioIngress(const config::FujiConfig& config,
                                 disk::DiskService& disks,
                                 io::IRequestHandler& routing)
    : _disk(disks)
    , _nio(routing)
    , _router(_disk, _nio, [this](const std::vector<std::uint8_t>& bytes) { send(bytes); })
{
    _router.set_high_speed_handler([this] { enter_high_speed(); });
    _nio.set_early_ack([this] { send_early_ack(); });
    _disk.set_early_read_ack([this] { send_early_ack(); });
    _owner_task = xTaskGetCurrentTaskHandle();
    _nio.set_clock([]() -> std::uint64_t { return static_cast<std::uint64_t>(esp_timer_get_time()); });
    // The network service says when a session of this client can be read (docs/atari/proc.md).
    routing.watchClient(&_nio, [this] {
        ++_proc_notices;
        _proc_wanted = true;
    });
    const auto& pins = pinmap();
    configure_sio_lines(pins);
    _cmd_pin = pins.sio.cmd;
    _proc_pin = pins.sio.proc;
    if (pins.sio.cmd >= 0 && pins.sio.cmd < GPIO_NUM_MAX) {
        const auto cmd = static_cast<gpio_num_t>(pins.sio.cmd);
        gpio_set_intr_type(cmd, GPIO_INTR_ANYEDGE);
        const esp_err_t service = gpio_install_isr_service(0);
        // The application may already own the global ISR service.  Handler
        // installation remains valid in that case and no second task/driver is
        // introduced.
        if ((service == ESP_OK || service == ESP_ERR_INVALID_STATE) &&
            gpio_isr_handler_add(cmd, cmd_edge_isr, this) == ESP_OK) {
            gpio_intr_enable(cmd);
        }
    }
    // UartChannel routes UART TX to the data-in pin and owns it from here. Do not touch the pin with
    // gpio_set_direction(): in ESP-IDF that re-routes it to a plain GPIO output and the UART can
    // never transmit, so the Atari would never see an ACK. On the v1 hardware TX feeds a 74LS07
    // buffer input, which wants a driven high (push-pull), as the legacy firmware does.
    _uart = std::make_unique<UartChannel>(sio_config(config.channel.uart), pins.sio.uart);
    // A second UART on the same RX pin (the GPIO matrix feeds one input to both), at the other speed.
    // Observation only for now: it never transmits and nothing acts on what it hears.
    if (pins.sio.uart.rx >= 0) {
        config::UartConfig alt_cfg = sio_config(config.channel.uart);
        alt_cfg.baudRate = sio_baud;
        _alt = std::make_unique<UartChannel>(alt_cfg, pins.sio.uart, UART_NUM_2, true);
        retune_alt();
    }
    // From here the ingress answers commands. sio.status compares this with the first CMD edge.
    _reply.ready_us = static_cast<std::uint32_t>(esp_timer_get_time());
    // The saved settings (fujinet.yaml, atari_sio): the gaps and window now, the speed index for 3F.
    io::AtariSioTuning tuning;
    tuning.ack_gap_us = config.atariSio.ackGapUs;
    tuning.data_gap_us = config.atariSio.dataGapUs;
    tuning.alt_window_us = config.atariSio.altWindowMs * 1000;
    tuning.ignore_write_glitches = config.atariSio.ignoreWriteGlitches;
    tuning.proc_notify = config.atariSio.procNotify;
    set_tuning(tuning);
    if (config.atariSio.hsioIndex >= 0) set_hsio_index(config.atariSio.hsioIndex);
}

AtariSioIngress::~AtariSioIngress() = default;

bool AtariSioIngress::supports_work_wait() const { return _uart && _uart->supports_readable_wait(); }
bool AtariSioIngress::wait_for_work(std::chrono::milliseconds timeout)
{
    if (_capture.needs_immediate_poll()) {
        esp_rom_delay_us(100);
        return true;
    }
    // An ISR task notification wakes the owner on COMMAND even though the
    // UART queue has not yet received a byte. Cap the fallback to one tick.
    const TickType_t ticks = timeout.count() > 0 ? 1 : 0;
    return ulTaskNotifyTake(pdTRUE, ticks) != 0;
}

// The ACK for the data frame of a native W, sent before the request runs: the Atari waits about 40 ms for
// it, and a request such as a TLS connect takes far longer. The COMPLETE follows from send().
void AtariSioIngress::send_early_ack()
{
    if (!_uart) return;
    _reply.record(static_cast<std::uint32_t>(esp_timer_get_time()), _capture.processing_start_us(),
                  _capture.processing_command(), _capture.processing_release_us());
    if (_capture.processing_command()) _txn.begin(_capture.last_frame()[1], _capture.processing_release_us());
    _uart->write(&sio_ack, 1);
    _uart->flushOutput();
    _early_ack_us = static_cast<std::uint32_t>(esp_timer_get_time());
    if (_early_ack_us == 0) _early_ack_us = 1;
}

void AtariSioIngress::send(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty() || !_uart) return;
    const auto trace_start = static_cast<std::uint32_t>(esp_timer_get_time());
    const auto trace_finish = [&] {
        const auto end = static_cast<std::uint32_t>(esp_timer_get_time());
        _capture.trace_reply(trace_start, end - trace_start, _uart->uart_config().baudRate,
                             static_cast<std::uint16_t>(bytes.size()), bytes[0],
                             bytes[0] == sio_ack && bytes.size() > 1 ? bytes[1] : 0);
    };
    // Timed before the write: this is when the reply starts, which is what the Atari waits for.
    _reply.record(static_cast<std::uint32_t>(esp_timer_get_time()), _capture.processing_start_us(),
                  _capture.processing_command() && _early_ack_us == 0, _capture.processing_release_us());
    if (_capture.processing_command() && _early_ack_us == 0)
        _txn.begin(_capture.last_frame()[1], _capture.processing_release_us());
    if (_early_ack_us != 0 && (bytes[0] == sio_complete || bytes[0] == sio_error)) {
        // The ACK went out before the request ran: keep the gap the Atari needs between ACK and COMPLETE.
        const std::uint32_t since = static_cast<std::uint32_t>(esp_timer_get_time()) - _early_ack_us;
        if (since < _tuning.ack_gap_us) esp_rom_delay_us(_tuning.ack_gap_us - since);
        _early_ack_us = 0;
        _uart->write(bytes.data(), 1);
        _uart->flushOutput();
        if (bytes.size() > 1) {
            if (_speed.high() && _tuning.data_gap_us) esp_rom_delay_us(_tuning.data_gap_us);
            const auto data_start = static_cast<std::uint32_t>(esp_timer_get_time());
            _uart->write(bytes.data() + 1, bytes.size() - 1);
            _uart->flushOutput();
            _reply.record_data(static_cast<std::uint32_t>(bytes.size() - 1),
                               static_cast<std::uint32_t>(esp_timer_get_time()) - data_start);
        }
        drop_tx_noise_edges();
        _txn.end(static_cast<std::uint32_t>(esp_timer_get_time()));
        trace_finish();
        return;
    }
    _early_ack_us = 0;
    if (bytes.size() >= 2 && bytes[0] == sio_ack && (bytes[1] == sio_complete || bytes[1] == sio_error)) {
        _uart->write(bytes.data(), 1);
        _uart->flushOutput();
        if (_tuning.ack_gap_us) esp_rom_delay_us(_tuning.ack_gap_us);
        std::size_t data_bytes = bytes.size() - 1;
        const std::uint8_t* data = bytes.data() + 1;
        if (bytes.size() > 2 && _speed.high()) {
            _uart->write(data, 1);
            _uart->flushOutput();
            if (_tuning.data_gap_us) esp_rom_delay_us(_tuning.data_gap_us);
            ++data;
            --data_bytes;
        }
        const auto data_start = static_cast<std::uint32_t>(esp_timer_get_time());
        _uart->write(data, data_bytes);
        _uart->flushOutput();
        _reply.record_data(static_cast<std::uint32_t>(data_bytes),
                           static_cast<std::uint32_t>(esp_timer_get_time()) - data_start);
        drop_tx_noise_edges();
        _txn.end(static_cast<std::uint32_t>(esp_timer_get_time()));
        trace_finish();
        return;
    }
    _uart->write(bytes.data(), bytes.size());
    _uart->flushOutput();
    drop_tx_noise_edges();
    _txn.end(static_cast<std::uint32_t>(esp_timer_get_time()));
    trace_finish();
}

// The Atari holds CMD high while we answer, so a CMD edge pending when a reply ends with CMD still high
// can only be noise from our own transmission. Left in, it aborts the next thing the capture is doing,
// and after an ACK that is the write's data frame: the Atari gets no ACK for it and times out (9 s on
// the OS SIO, 40 ms plus a retry on the high-speed driver). A real assertion leaves the pin low and is kept.
void AtariSioIngress::drop_tx_noise_edges()
{
    if (_cmd_pin < 0 || _cmd_pin >= GPIO_NUM_MAX) return;
    if (gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0) return;
    _tx_noise_edges += _boundary.consume();
}

io::AtariSioStats AtariSioIngress::stats() const
{
    io::AtariSioStats out;
    out.cmd_edges = _boundary.total_edges();
    out.first_edge_us = _boundary.first_edge_us();
    out.cmd_line_low = _cmd_pin >= 0 && _cmd_pin < GPIO_NUM_MAX &&
                       gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0;
    out.uart_rx_errors = _uart ? _uart->rx_errors() : 0;
    if (_uart) {
        out.uart_baud_set = _uart->uart_config().baudRate;
        out.uart_baud_hw = _uart->getBaudrate();
    }
    out.capture = _capture.stats();
    out.router = _router.stats();
    out.reply = _reply;
    out.poll_gap_us_max = _poll_gap_us_max;
    out.poll_gap_at_us = _poll_gap_at_us;
    out.stale_replies_dropped = _nio.stale_replies_dropped();
    out.hsio_index = _speed.index();
    out.hsio_high = _speed.high();
    out.hsio_switches = _hsio_switches;
    out.hsio_fallbacks = _hsio_fallbacks;
    out.hsio_fallback_enabled = _speed.fallback();
    out.tuning = _tuning;
    out.txn = _txn;
    out.tx_noise_edges = _tx_noise_edges;
    out.proc_available = _proc_pin >= 0 && _proc_pin < GPIO_NUM_MAX;
    out.proc_low = out.proc_available && gpio_get_level(static_cast<gpio_num_t>(_proc_pin)) == 0;
    out.proc_asserts = _proc_asserts;
    out.proc_notices = _proc_notices;
    out.proc_pulses = _proc_pulses;
    if (_alt) {
        out.alt_baud = _alt->uart_config().baudRate;
        out.alt_valid_frames = _alt_valid;
        out.alt_ambiguous_dropped = _alt_ambiguous_dropped;
        out.alt_last_frame = _alt_last;
    }
    return out;
}

bool AtariSioIngress::set_proc(bool asserted)
{
    if (_proc_pin < 0 || _proc_pin >= GPIO_NUM_MAX) return false;
    if (asserted) ++_proc_asserts;
    gpio_set_level(static_cast<gpio_num_t>(_proc_pin), asserted ? 0 : 1);
    return true;
}

bool AtariSioIngress::pulse_proc(unsigned count, unsigned low_ms, unsigned high_ms)
{
    if (_proc_pin < 0 || _proc_pin >= GPIO_NUM_MAX) return false;
    for (unsigned i = 0; i < count; ++i) {
        set_proc(true);
        vTaskDelay(pdMS_TO_TICKS(low_ms) ? pdMS_TO_TICKS(low_ms) : 1);
        set_proc(false);
        vTaskDelay(pdMS_TO_TICKS(high_ms) ? pdMS_TO_TICKS(high_ms) : 1);
    }
    return true;
}

bool AtariSioIngress::set_hsio_index(int index, bool fallback)
{
    if (!_speed.set_index(index)) return false;
    _speed.set_fallback(fallback);
    _disk.set_hsio_index(index);
    _nio.set_hsio_index(index);
    // Off while the UART is fast, or a new index while it is: the UART follows what is now set.
    if (_uart && _uart->uart_config().baudRate != _speed.baud()) {
        _uart->discardInput();
        _uart->setBaudrate(_speed.baud());
    }
    retune_alt();
    return true;
}

// Called right after the 3F reply has been sent and flushed, so it left at the standard speed.
void AtariSioIngress::enter_high_speed()
{
    if (!_uart || !_speed.enabled()) return;
    _speed.enter_high();
    _uart->discardInput();
    _uart->setBaudrate(_speed.baud());
    ++_hsio_switches;
    retune_alt();
}

void AtariSioIngress::leave_high_speed()
{
    const bool was_high = _speed.high();
    _speed.leave_high();
    _capture.clear_bad_frames();
    if (!_uart) return;
    _uart->discardInput();
    _uart->setBaudrate(_speed.baud());
    if (was_high) ++_hsio_fallbacks;
    retune_alt();
}

void AtariSioIngress::retune_alt()
{
    if (!_alt) return;
    const std::uint32_t other = _speed.high() ? sio_baud : io::AtariSioSpeed::baud_for_index(
        static_cast<unsigned>(_speed.enabled() ? _speed.index() : 0));
    if (_alt->uart_config().baudRate != other) {
        _alt->discardInput();
        _alt->setBaudrate(other);
    }
}

// Slides a 5-byte window over what the second listener hears and counts the windows that are a command
// frame: a device we serve and a good checksum. Bytes of the wrong speed rarely pass both tests.
void AtariSioIngress::poll_alt()
{
    if (!_alt) return;
    std::uint8_t bytes[64];
    while (_alt->available()) {
        const auto count = _alt->read(bytes, sizeof(bytes));
        for (std::size_t i = 0; i < count; ++i) {
            if (_alt_used == _alt_window.size()) {
                std::copy(_alt_window.begin() + 1, _alt_window.end(), _alt_window.begin());
                --_alt_used;
            }
            _alt_window[_alt_used++] = bytes[i];
            if (_alt_used < _alt_window.size()) continue;
            const auto device = _alt_window[0];
            if ((device < 0x31 || device > 0x38) && device != 0x7f) continue;
            if (io::AtariSioNioFrontend::checksum(_alt_window.data(), 4) != _alt_window[4]) continue;
            ++_alt_valid;
            _alt_last = _alt_window;
            _alt_pending = true;
            _alt_found_us = static_cast<std::uint32_t>(esp_timer_get_time());
            _alt_used = 0;
        }
    }
}

void AtariSioIngress::toggle_speed()
{
    if (_speed.high()) {
        leave_high_speed();
    } else if (_speed.enabled()) {
        _capture.clear_bad_frames();
        _speed.enter_high();
        _uart->discardInput();
        _uart->setBaudrate(_speed.baud());
        ++_hsio_switches;
        retune_alt();
    }
}

// A frame heard at the other speed only counts for the command that has just ended, so it expires fast.
bool AtariSioIngress::take_alt_frame(std::array<std::uint8_t, 5>& frame, bool unambiguous)
{
    if (!_alt_pending) return false;
    _alt_pending = false;
    if (!unambiguous) ++_alt_ambiguous_dropped;
    if (!unambiguous || !_speed.enabled() || static_cast<std::uint32_t>(esp_timer_get_time()) - _alt_found_us > _tuning.alt_window_us) return false;
    frame = _alt_last;
    toggle_speed();
    return true;
}

std::uint32_t AtariSioIngress::probe_baud(std::uint32_t target)
{
    if (!_uart || target == 0) return 0;
    const auto original = _uart->uart_config().baudRate;
    _uart->setBaudrate(target);
    const auto hardware = _uart->getBaudrate();
    _uart->setBaudrate(original);
    return hardware;
}

void AtariSioIngress::reset_stats()
{
    _boundary.reset_total_edges();
    _capture.reset_stats();
    _router.reset_stats();
    _reply.reset();
    _txn.reset();
    _hsio_switches = 0;
    _hsio_fallbacks = 0;
    _alt_valid = 0;
    _alt_ambiguous_dropped = 0;
    _alt_last = {};
    _tx_noise_edges = 0;
    _poll_gap_us_max = 0;
    _poll_gap_at_us = 0;
}

void AtariSioIngress::note_cmd_edge() noexcept
{
    _boundary.note_cmd_edge(static_cast<std::uint32_t>(esp_timer_get_time()));
    BaseType_t woken = pdFALSE;
    if (_owner_task) vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(_owner_task), &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

namespace {
constexpr std::uint32_t proc_pulse_us = 500;       // how long PROC is held low: the PIA needs a few microseconds
constexpr std::uint32_t proc_min_gap_us = 5000;    // notices closer together than this share one pulse
}

// The notice from the network service is a hint that a read would now succeed. Pulse PROC for it only
// when the Atari is not in the middle of a transaction (CMD high, no frame or write data being received),
// so the pulse cannot disturb one, and not more often than proc_min_gap_us.
void AtariSioIngress::service_proc_notice()
{
    if (!_proc_wanted) return;
    if (!_tuning.proc_notify) {
        _proc_wanted = false;
        return;
    }
    if (_proc_pin < 0 || _proc_pin >= GPIO_NUM_MAX || _capture.needs_immediate_poll()) return;
    if (gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0) return;
    const auto now = static_cast<std::uint32_t>(esp_timer_get_time());
    if (_proc_last_pulse_us != 0 && now - _proc_last_pulse_us < proc_min_gap_us) return;
    _proc_wanted = false;
    set_proc(true);
    esp_rom_delay_us(proc_pulse_us);
    set_proc(false);
    _proc_last_pulse_us = static_cast<std::uint32_t>(esp_timer_get_time());
    ++_proc_pulses;
}

void AtariSioIngress::poll()
{
    if (!_uart || _cmd_pin < 0 || _cmd_pin >= GPIO_NUM_MAX) return;
    const auto now = static_cast<std::uint32_t>(esp_timer_get_time());
    const auto edges = _boundary.total_edges();
    // The gap is from the end of one poll to the start of the next, so it excludes serving a command:
    // sending a 256-byte sector blocks the poll for about 170 ms at 19200 baud, and the line is busy
    // then anyway. Only a gap in which the Atari signalled CMD can have cost a command.
    if (_last_poll_us != 0 && edges != _edges_at_last_poll && now - _last_poll_us > _poll_gap_us_max) {
        _poll_gap_us_max = now - _last_poll_us;
        _poll_gap_at_us = _last_poll_us;
    }
    poll_alt();
    _capture.poll(_boundary, _router, *_uart,
                  [this] { return gpio_get_level(static_cast<gpio_num_t>(_cmd_pin)) == 0; },
                  [] { return esp_timer_get_time(); },
                  [this](std::array<std::uint8_t, 5>& frame, bool unambiguous) { return take_alt_frame(frame, unambiguous); });
    // The legacy firmware's rule, kept as the backup for when the second listener does not catch it: two
    // unreadable command frames in a row mean the Atari is at the other speed, so toggle.
    if (_speed.enabled() && _speed.fallback() &&
        _capture.bad_frames_in_a_row() >= io::AtariSioSpeed::fallback_after_bad_frames) {
        toggle_speed();
    }
    service_proc_notice();
    _last_poll_us = static_cast<std::uint32_t>(esp_timer_get_time());
    _edges_at_last_poll = _boundary.total_edges();
}

} // namespace fujinet::platform::esp32

#endif
