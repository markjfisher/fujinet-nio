#include "fujinet/platform/esp32/atari_sio_fujibus_channel.h"

#include "fujinet/core/logging.h"
#include "fujinet/io/transport/atari_sio_fujibus_framer.h"
#include "fujinet/platform/esp32/pinmap.h"
#include "fujinet/platform/esp32/uart_channel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include "driver/gpio.h"
}

namespace fujinet::platform::esp32 {

namespace {

static constexpr const char* TAG = "sio-fb";
static constexpr std::uint32_t ATARI_SIO_BAUD = 19200;
static constexpr std::size_t IO_BUF_SIZE = 256;

bool valid_gpio(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

void configure_input_pin(const char* name, int pin)
{
    if (!valid_gpio(pin)) {
        return;
    }
    const auto gpio = static_cast<gpio_num_t>(pin);
    const esp_err_t err = gpio_set_direction(gpio, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        FN_LOGW(TAG, "failed to configure SIO %s GPIO%d as input: %d", name, pin, err);
        return;
    }
    FN_LOGI(TAG, "SIO %s GPIO%d input", name, pin);
}

void configure_open_drain_released_pin(const char* name, int pin)
{
    if (!valid_gpio(pin)) {
        return;
    }
    const auto gpio = static_cast<gpio_num_t>(pin);
    gpio_set_level(gpio, 1);
    esp_err_t err = gpio_set_direction(gpio, GPIO_MODE_OUTPUT_OD);
    if (err != ESP_OK) {
        FN_LOGW(TAG, "failed to configure SIO %s GPIO%d as open-drain output: %d", name, pin, err);
        return;
    }
    err = gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        FN_LOGD(TAG, "SIO %s GPIO%d pull-up not enabled: %d", name, pin, err);
    }
    FN_LOGI(TAG, "SIO %s GPIO%d open-drain released", name, pin);
}

void configure_sio_control_lines(const PinMap& pins)
{
    configure_input_pin("CMD", pins.sio.cmd);
    configure_input_pin("MTR", pins.sio.mtr);
    configure_input_pin("CKO", pins.sio.cko);

    configure_open_drain_released_pin("INT", pins.sio.int_pin);
    configure_open_drain_released_pin("PROC", pins.sio.proc);
    configure_open_drain_released_pin("CKI", pins.sio.cki);
}

config::UartConfig sio_uart_config(config::UartConfig cfg)
{
    // FujiConfig's generic UART default is 115200 for RS-232-style builds.
    // Atari OS SIO calls use the standard SIO rate unless config explicitly
    // provides a different value.
    if (cfg.baudRate == config::UartConfig{}.baudRate) {
        cfg.baudRate = ATARI_SIO_BAUD;
    }
    cfg.dataBits = 8;
    cfg.parity = config::UartParity::None;
    cfg.stopBits = config::UartStopBits::One;
    cfg.flowControl = config::UartFlowControl::None;
    return cfg;
}

class AtariSioFujiBusChannel final : public fujinet::io::Channel {
public:
    explicit AtariSioFujiBusChannel(const config::FujiConfig& config)
    {
        const PinMap& pins = pinmap();
        configure_sio_control_lines(pins);
        _uart = std::make_unique<UartChannel>(
            sio_uart_config(config.channel.uart),
            pins.sio.uart);

        if (valid_gpio(pins.sio.uart.tx)) {
            const auto tx = static_cast<gpio_num_t>(pins.sio.uart.tx);
            gpio_set_pull_mode(tx, GPIO_PULLUP_ONLY);
            const esp_err_t err = gpio_set_direction(tx, GPIO_MODE_OUTPUT_OD);
            if (err != ESP_OK) {
                FN_LOGW(TAG, "failed to configure SIO DATA OUT GPIO%d as open-drain: %d",
                        pins.sio.uart.tx,
                        err);
            } else {
                FN_LOGI(TAG, "SIO DATA OUT GPIO%d open-drain", pins.sio.uart.tx);
            }
        }
    }

    bool available() override
    {
        pump();
        return _framer.has_request();
    }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (!buffer || maxLen == 0) {
            return 0;
        }
        pump();
        return _framer.read_request(buffer, maxLen);
    }

    void write(const std::uint8_t* buffer, std::size_t len) override
    {
        if (!buffer || len == 0) {
            return;
        }
        _framer.queue_response(buffer, len);
        flush_output();
    }

private:
    void pump()
    {
        if (!_uart) {
            return;
        }

        std::uint8_t buf[IO_BUF_SIZE];
        while (_uart->available()) {
            const std::size_t n = _uart->read(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            _framer.ingest(buf, n);
        }
        flush_output();
    }

    void flush_output()
    {
        if (!_uart) {
            return;
        }

        std::uint8_t buf[IO_BUF_SIZE];
        while (_framer.has_output()) {
            const std::size_t n = _framer.read_output(buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            _uart->write(buf, n);
            _uart->flushOutput();
        }
    }

    std::unique_ptr<UartChannel> _uart;
    fujinet::io::transport::AtariSioFujiBusFramer _framer;
};

} // namespace

std::unique_ptr<fujinet::io::Channel>
create_atari_sio_fujibus_channel(const fujinet::config::FujiConfig& config)
{
    return std::make_unique<AtariSioFujiBusChannel>(config);
}

} // namespace fujinet::platform::esp32
