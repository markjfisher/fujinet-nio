#include "fujinet/platform/esp32/uart_channel.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/esp32/pinmap.h"

#include <cstddef>
#include <algorithm>

extern "C" {
#include "driver/uart.h"
#include "soc/uart_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
}

namespace fujinet::platform::esp32 {

namespace {

static const char* TAG = "uart_channel";

static constexpr int UART_RX_BUF_SIZE = 2048;
static constexpr int UART_TX_BUF_SIZE = 0;  // 0 = TX buffer not used, blocking write
static constexpr int UART_QUEUE_SIZE = 10;
static constexpr int MAX_FLUSH_WAIT_TICKS = 200;
/// HW flow: UART ISR asserts RTS when RX FIFO bytes exceed this threshold.
static constexpr int UART_RX_FLOW_THRESH = 112;

static uart_word_length_t map_data_bits(int bits)
{
    switch (bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
        return UART_DATA_8_BITS;
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t map_parity(config::UartParity p)
{
    switch (p) {
    case config::UartParity::Even:
        return UART_PARITY_EVEN;
    case config::UartParity::Odd:
        return UART_PARITY_ODD;
    case config::UartParity::None:
    default:
        return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t map_stop_bits(config::UartStopBits s)
{
    switch (s) {
    case config::UartStopBits::Two:
        return UART_STOP_BITS_2;
    case config::UartStopBits::OnePointFive:
        return UART_STOP_BITS_1_5;
    case config::UartStopBits::One:
    default:
        return UART_STOP_BITS_1;
    }
}

} // namespace

UartChannel::UartChannel(const config::UartConfig& uart_cfg)
    : _uart_cfg(uart_cfg)
{
    _initialized = initialize();
    if (!_initialized) {
        FN_LOGE(TAG, "Failed to initialize UartChannel");
    }
}

UartChannel::~UartChannel()
{
    if (_initialized) {
        uart_driver_delete(_uart_port);
        _initialized = false;
    }

    _uart_queue = nullptr;
}

bool UartChannel::initialize()
{
    const UartPins uart_pins = pinmap().primaryUart();

    if (uart_pins.rx < 0 || uart_pins.tx < 0) {
        FN_LOGE(TAG, "No valid UART pins configured in pinmap");
        return false;
    }

    FN_LOGI(TAG, "Using UART pins: RX=%d, TX=%d", uart_pins.rx, uart_pins.tx);

    _uart_port = UART_NUM_1;

    int baud = static_cast<int>(_uart_cfg.baudRate);
    if (baud <= 0) {
        FN_LOGW(TAG, "Invalid UART baud %u, using 115200", static_cast<unsigned>(_uart_cfg.baudRate));
        baud = 115200;
        _uart_cfg.baudRate = 115200;
    }

    int data_bits = _uart_cfg.dataBits;
    if (data_bits < 5 || data_bits > 8) {
        FN_LOGW(TAG, "Invalid UART data_bits %d, using 8", data_bits);
        data_bits = 8;
        _uart_cfg.dataBits = 8;
    }

    const PinMap& pm = pinmap();
    int rts_pin = UART_PIN_NO_CHANGE;
    int cts_pin = UART_PIN_NO_CHANGE;
    uart_hw_flowcontrol_t flow = UART_HW_FLOWCTRL_DISABLE;

    if (_uart_cfg.flowControl == config::UartFlowControl::RtsCts) {
        if (pm.rs232.rts >= 0 && pm.rs232.cts >= 0) {
            rts_pin = pm.rs232.rts;
            cts_pin = pm.rs232.cts;
#if defined(UART_HW_FLOWCTRL_CTS_RTS)
            flow = UART_HW_FLOWCTRL_CTS_RTS;
#else
            flow = static_cast<uart_hw_flowcontrol_t>(UART_HW_FLOWCTRL_RTS | UART_HW_FLOWCTRL_CTS);
#endif
            FN_LOGI(TAG, "UART hardware flow control RTS=%d CTS=%d", rts_pin, cts_pin);
        } else {
            FN_LOGW(TAG,
                    "flow_control rts_cts requested but board pinmap has no RS232 RTS/CTS; using none");
            _uart_cfg.flowControl = config::UartFlowControl::None;
        }
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = baud;
    uart_config.data_bits = map_data_bits(data_bits);
    uart_config.parity = map_parity(_uart_cfg.parity);
    uart_config.stop_bits = map_stop_bits(_uart_cfg.stopBits);
    uart_config.flow_ctrl = flow;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    if (flow != UART_HW_FLOWCTRL_DISABLE) {
        uart_config.rx_flow_ctrl_thresh = UART_RX_FLOW_THRESH;
    }

    esp_err_t err = uart_param_config(_uart_port, &uart_config);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_param_config failed: %d", err);
        return false;
    }

    err = uart_set_pin(_uart_port, uart_pins.tx, uart_pins.rx, rts_pin, cts_pin);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_set_pin failed: %d", err);
        return false;
    }

    err = uart_driver_install(
        _uart_port,
        UART_RX_BUF_SIZE,
        UART_TX_BUF_SIZE,
        UART_QUEUE_SIZE,
        &_uart_queue,
        0);
    if (err != ESP_OK) {
        FN_LOGE(TAG, "uart_driver_install failed: %d", err);
        return false;
    }

    FN_LOGI(TAG,
            "UartChannel UART%d baud=%d data=%d parity=%d stop=%d flow=%d",
            static_cast<int>(_uart_port),
            baud,
            data_bits,
            static_cast<int>(_uart_cfg.parity),
            static_cast<int>(_uart_cfg.stopBits),
            static_cast<int>(_uart_cfg.flowControl));
    return true;
}

void UartChannel::updateFIFO()
{
    if (!_initialized || !_uart_queue) {
        return;
    }

    uart_event_t event;

    while (xQueueReceive(_uart_queue, &event, 1))
    {
        switch (event.type) {
        case UART_DATA:
            {
                size_t old_len = _fifo.size();
                _fifo.resize(old_len + event.size);
                int result = uart_read_bytes(_uart_port, &_fifo[old_len], event.size, 0);
                if (result < 0) {
                    result = 0;
                }
                _fifo.resize(old_len + result);
            }
            break;

        case UART_FIFO_OVF:
            FN_LOGW(TAG, "UART FIFO overflow");
            uart_flush_input(_uart_port);
            break;

        case UART_BUFFER_FULL:
            FN_LOGW(TAG, "UART buffer full");
            uart_flush_input(_uart_port);
            break;

        case UART_BREAK:
            FN_LOGI(TAG, "UART break detected");
            break;

        case UART_PARITY_ERR:
            FN_LOGW(TAG, "UART parity error");
            break;

        case UART_FRAME_ERR:
            FN_LOGW(TAG, "UART frame error");
            break;

        case UART_PATTERN_DET:
            break;

        default:
            FN_LOGW(TAG, "Unknown UART event: %d", event.type);
            break;
        }
    }
}

bool UartChannel::available()
{
    if (!_initialized) {
        return false;
    }

    updateFIFO();

    return !_fifo.empty();
}

std::size_t UartChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    if (!_initialized || buffer == nullptr || maxLen == 0) {
        return 0;
    }

    updateFIFO();

    std::size_t to_copy = (maxLen < _fifo.size()) ? maxLen : _fifo.size();
    if (to_copy == 0) {
        return 0;
    }

    std::copy(_fifo.begin(), _fifo.begin() + to_copy, buffer);

    _fifo.erase(_fifo.begin(), _fifo.begin() + to_copy);

    return to_copy;
}

void UartChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    if (!_initialized || buffer == nullptr || len == 0) {
        return;
    }

    int bytes_written = uart_write_bytes(_uart_port, buffer, len);
    if (bytes_written < 0) {
        FN_LOGE(TAG, "uart_write_bytes failed: %d", bytes_written);
        return;
    }

    if (static_cast<std::size_t>(bytes_written) != len) {
        FN_LOGW(TAG, "Partial write: %d of %zu bytes", bytes_written, len);
    }
}

void UartChannel::flushOutput()
{
    if (!_initialized) {
        return;
    }
    uart_wait_tx_done(_uart_port, MAX_FLUSH_WAIT_TICKS);
}

uint32_t UartChannel::getBaudrate()
{
    if (!_initialized) {
        return 0;
    }
    uint32_t baud = 0;
    uart_get_baudrate(_uart_port, &baud);
    return baud;
}

void UartChannel::setBaudrate(uint32_t baud)
{
    if (!_initialized) {
        return;
    }
    uart_set_baudrate(_uart_port, baud);
    _uart_cfg.baudRate = baud;
}

} // namespace fujinet::platform::esp32
