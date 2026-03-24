#include "fujinet/diag/diagnostic_provider.h"

#if defined(FN_PLATFORM_POSIX)

namespace fujinet::diag {

std::unique_ptr<IDiagnosticProvider> create_uart_channel_diagnostic_provider(
    fujinet::io::Channel* /*channel*/,
    fujinet::io::FujiDevice* /*fuji*/)
{
    return nullptr;
}

} // namespace fujinet::diag

#else

#include "fujinet/config/fuji_config.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/core/channel.h"
#include "fujinet/io/devices/fuji_device.h"
#include "fujinet/platform/esp32/uart_channel.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::diag {

namespace detail {

static const char* parity_str(config::UartParity p)
{
    switch (p) {
    case config::UartParity::Even:
        return "even";
    case config::UartParity::Odd:
        return "odd";
    case config::UartParity::None:
    default:
        return "none";
    }
}

static const char* stop_str(config::UartStopBits s)
{
    switch (s) {
    case config::UartStopBits::Two:
        return "2";
    case config::UartStopBits::OnePointFive:
        return "1.5";
    case config::UartStopBits::One:
    default:
        return "1";
    }
}

static const char* flow_str(config::UartFlowControl f)
{
    switch (f) {
    case config::UartFlowControl::RtsCts:
        return "rts_cts";
    case config::UartFlowControl::None:
    default:
        return "none";
    }
}

static config::UartParity parse_parity(std::string_view raw)
{
    if (ascii_iequals(raw, "even")) {
        return config::UartParity::Even;
    }
    if (ascii_iequals(raw, "odd")) {
        return config::UartParity::Odd;
    }
    return config::UartParity::None;
}

static config::UartStopBits parse_stop(std::string_view raw)
{
    if (ascii_iequals(raw, "2") || ascii_iequals(raw, "two")) {
        return config::UartStopBits::Two;
    }
    if (ascii_iequals(raw, "1.5") || ascii_iequals(raw, "1_5") || ascii_iequals(raw, "one_point_five")) {
        return config::UartStopBits::OnePointFive;
    }
    return config::UartStopBits::One;
}

static config::UartFlowControl parse_flow(std::string_view raw)
{
    if (ascii_iequals(raw, "rts_cts") || ascii_iequals(raw, "rts-cts") || ascii_iequals(raw, "hw")) {
        return config::UartFlowControl::RtsCts;
    }
    return config::UartFlowControl::None;
}

static fujinet::platform::esp32::UartChannel* as_uart(fujinet::io::Channel* ch)
{
    if (ch == nullptr) {
        return nullptr;
    }
    return dynamic_cast<fujinet::platform::esp32::UartChannel*>(ch);
}

class UartChannelDiagnosticProvider final : public IDiagnosticProvider {
public:
    UartChannelDiagnosticProvider(fujinet::io::Channel* channel, fujinet::io::FujiDevice* fuji)
        : _channel(channel)
        , _fuji(fuji)
    {}

    std::string_view provider_id() const noexcept override { return "uart"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "uart.status",
            .summary = "show host UART config (FujiBus channel) and hardware baud",
            .usage = "uart.status",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "uart.baud",
            .summary = "set line baud rate (updates stored config when FujiDevice present)",
            .usage = "uart.baud <rate>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "uart.set",
            .summary = "set one field: baud_rate|data_bits|parity|stop_bits|flow_control",
            .usage = "uart.set <field> <value>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "uart.save",
            .summary = "write current UART settings into fujinet.yaml (requires FujiDevice)",
            .usage = "uart.save",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "uart.status") {
            return cmd_status();
        }
        if (cmd == "uart.baud") {
            return cmd_baud(args);
        }
        if (cmd == "uart.set") {
            return cmd_set(args);
        }
        if (cmd == "uart.save") {
            return cmd_save();
        }

        return DiagResult::not_found("unknown uart command");
    }

private:
    DiagResult cmd_status()
    {
        auto* uart = as_uart(_channel);
        if (!uart) {
            return DiagResult::not_ready("UartChannel not active (e.g. USB CDC transport)");
        }

        const config::UartConfig& c = uart->uart_config();
        const std::uint32_t hw = uart->getBaudrate();

        std::string text;
        text += "configured_baud_rate: ";
        text += std::to_string(c.baudRate);
        text += "\r\n";
        text += "hardware_baud_rate: ";
        text += std::to_string(hw);
        text += "\r\n";
        text += "data_bits: ";
        text += std::to_string(c.dataBits);
        text += "\r\n";
        text += "parity: ";
        text += parity_str(c.parity);
        text += "\r\n";
        text += "stop_bits: ";
        text += stop_str(c.stopBits);
        text += "\r\n";
        text += "flow_control: ";
        text += flow_str(c.flowControl);
        text += "\r\n";
        return DiagResult::ok(std::move(text));
    }

    DiagResult cmd_baud(const DiagArgsView& args)
    {
        auto* uart = as_uart(_channel);
        if (!uart) {
            return DiagResult::not_ready("UartChannel not active");
        }
        if (args.argv.size() < 2) {
            return DiagResult::invalid_args("usage: uart.baud <rate>");
        }
        std::uint32_t baud = 0;
        if (!parse_decimal_u32(args.argv[1], baud) || baud == 0) {
            return DiagResult::invalid_args("invalid baud rate");
        }
        uart->setBaudrate(baud);
        if (_fuji) {
            _fuji->config_mut().channel.uart.baudRate = baud;
        }
        return DiagResult::ok("baud set to " + std::to_string(baud));
    }

    DiagResult cmd_set(const DiagArgsView& args)
    {
        auto* uart = as_uart(_channel);
        if (!uart) {
            return DiagResult::not_ready("UartChannel not active");
        }
        if (args.argv.size() < 3) {
            return DiagResult::invalid_args(
                "usage: uart.set <baud_rate|data_bits|parity|stop_bits|flow_control> <value>");
        }

        config::UartConfig next = uart->uart_config();
        const std::string_view field = args.argv[1];
        const std::string_view val = args.argv[2];

        if (ascii_iequals(field, "baud_rate")) {
            std::uint32_t baud = 0;
            if (!parse_decimal_u32(val, baud) || baud == 0) {
                return DiagResult::invalid_args("invalid baud_rate");
            }
            next.baudRate = baud;
        } else if (ascii_iequals(field, "data_bits")) {
            std::uint32_t ub = 0;
            if (!parse_decimal_u32(val, ub) || ub < 5 || ub > 8) {
                return DiagResult::invalid_args("data_bits must be 5..8");
            }
            next.dataBits = static_cast<int>(ub);
        } else if (ascii_iequals(field, "parity")) {
            next.parity = parse_parity(val);
        } else if (ascii_iequals(field, "stop_bits")) {
            next.stopBits = parse_stop(val);
        } else if (ascii_iequals(field, "flow_control")) {
            next.flowControl = parse_flow(val);
        } else {
            return DiagResult::invalid_args("unknown field (see uart.set usage)");
        }

        if (!uart->reconfigure(next)) {
            return DiagResult::error("uart reconfigure failed");
        }
        if (_fuji) {
            _fuji->config_mut().channel.uart = uart->uart_config();
        }
        return DiagResult::ok("uart updated (see uart.status)");
    }

    DiagResult cmd_save()
    {
        auto* uart = as_uart(_channel);
        if (!uart) {
            return DiagResult::not_ready("UartChannel not active");
        }
        if (!_fuji) {
            return DiagResult::not_ready("FujiDevice not available");
        }
        auto* store = _fuji->config_store();
        if (!store) {
            return DiagResult::not_ready("config store not available");
        }
        _fuji->config_mut().channel.uart = uart->uart_config();
        store->save(_fuji->config());
        return DiagResult::ok("saved channel.uart to config store");
    }

    fujinet::io::Channel* _channel;
    fujinet::io::FujiDevice* _fuji;
};

} // namespace detail

std::unique_ptr<IDiagnosticProvider> create_uart_channel_diagnostic_provider(
    fujinet::io::Channel* channel,
    fujinet::io::FujiDevice* fuji)
{
    if (detail::as_uart(channel) == nullptr) {
        return nullptr;
    }
    return std::make_unique<detail::UartChannelDiagnosticProvider>(channel, fuji);
}

} // namespace fujinet::diag

#endif
