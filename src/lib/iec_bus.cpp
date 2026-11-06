#include "fujinet/io/iec_bus.h"

namespace fujinet::io {

namespace {

// Primary commands: the top three bits, with the device number below.
constexpr std::uint8_t command_mask = 0xE0;
constexpr std::uint8_t device_mask_bits = 0x1F;
constexpr std::uint8_t primary_listen = 0x20;
constexpr std::uint8_t primary_talk = 0x40;
constexpr std::uint8_t primary_unlisten = 0x3F;
constexpr std::uint8_t primary_untalk = 0x5F;

// Secondary commands. The channel is the low four bits; a data secondary ($60-$7F)
// ignores bit 4, as a stock drive does.
constexpr std::uint8_t channel_mask = 0x0F;
constexpr std::uint8_t secondary_data = 0x60;
constexpr std::uint8_t secondary_type_mask = 0xF0;
constexpr std::uint8_t secondary_close = 0xE0;
constexpr std::uint8_t secondary_open = 0xF0;

} // namespace

bool IecBus::attach(std::uint8_t device, IIecDevice& iecDevice)
{
    if (device > max_device) {
        return false;
    }
    _devices[device] = &iecDevice;
    return true;
}

IIecDevice* IecBus::at(int device) const
{
    if (device < 0 || device > max_device) {
        return nullptr;
    }
    IIecDevice* found = _devices[static_cast<std::size_t>(device)];
    return (found && found->present()) ? found : nullptr;
}

std::uint32_t IecBus::device_mask() const
{
    std::uint32_t mask = 0;
    for (int device = 0; device <= max_device; ++device) {
        if (at(device)) {
            mask |= std::uint32_t{1} << device;
        }
    }
    return mask;
}

void IecBus::end_listener()
{
    IIecDevice* device = at(_listener);
    if (device) {
        if (_opening) {
            device->open(_openChannel, _name.data(), _name.size());
        } else if (_listening) {
            device->unlisten();
        }
    }
    _listener = none;
    _listening = false;
    _opening = false;
    _name.clear();
}

void IecBus::end_talker()
{
    IIecDevice* device = at(_talker);
    if (device && _talking) {
        device->untalk();
    }
    _talker = none;
    _talking = false;
}

IecAtnStatus IecBus::command(std::uint8_t byte)
{
    if (byte == primary_unlisten) {
        end_listener();
        return IecAtnStatus::Acknowledged;
    }
    if (byte == primary_untalk) {
        end_talker();
        return IecAtnStatus::Acknowledged;
    }

    // Addressing a device ends both roles: the computer sends LISTEN without UNTALK
    // when RUN/STOP stops a LOAD, and sends nothing at all after FILE NOT FOUND.
    const std::uint8_t primary = byte & command_mask;
    if (primary == primary_listen) {
        end_talker();
        end_listener();
        _listener = byte & device_mask_bits;
        return at(_listener) ? IecAtnStatus::Acknowledged : IecAtnStatus::NotPresent;
    }
    if (primary == primary_talk) {
        end_talker();
        end_listener();
        _talker = byte & device_mask_bits;
        return at(_talker) ? IecAtnStatus::Acknowledged : IecAtnStatus::NotPresent;
    }

    const std::uint8_t channel = byte & channel_mask;
    const bool isData = primary == secondary_data;

    if (_talker != none && !_talking) {
        IIecDevice* device = at(_talker);
        if (!device) {
            return IecAtnStatus::NotPresent;
        }
        if (isData) {
            device->talk(channel);
            _talking = true;
        }
        return IecAtnStatus::Acknowledged;
    }

    if (_listener != none && !_listening && !_opening) {
        IIecDevice* device = at(_listener);
        if (!device) {
            return IecAtnStatus::NotPresent;
        }
        const std::uint8_t secondary = byte & secondary_type_mask;
        if (isData) {
            device->listen(channel);
            _listening = true;
        } else if (secondary == secondary_close) {
            device->close(channel);
        } else if (secondary == secondary_open) {
            _opening = true;
            _openChannel = channel;
            _name.clear();
        }
        return IecAtnStatus::Acknowledged;
    }

    return IecAtnStatus::NotPresent;
}

IecAtnStatus IecBus::atn(const std::uint8_t* bytes, std::size_t length)
{
    if (!bytes || length == 0) {
        return IecAtnStatus::NotPresent;
    }
    IecAtnStatus status = IecAtnStatus::Acknowledged;
    for (std::size_t i = 0; i < length; ++i) {
        if (command(bytes[i]) == IecAtnStatus::NotPresent) {
            status = IecAtnStatus::NotPresent;
        }
    }
    return status;
}

std::size_t IecBus::write(const std::uint8_t* data, std::size_t length, bool eoi)
{
    IIecDevice* device = at(_listener);
    if (!device || !data) {
        return 0;
    }
    if (_opening) {
        // Every name byte is taken, however long: a refusal here would read as an
        // error on the computer even when the file then opens.
        for (std::size_t i = 0; i < length && _name.size() < max_name_bytes; ++i) {
            _name.push_back(data[i]);
        }
        return length;
    }
    // A listener addressed with no secondary at all takes data on channel 0.
    if (!_listening) {
        device->listen(0);
        _listening = true;
    }
    return device->receive(data, length, eoi);
}

IecReadResult IecBus::read(std::uint8_t* dst, std::size_t maxBytes)
{
    IIecDevice* device = at(_talker);
    if (!device || !dst) {
        return IecReadResult{0, true};
    }
    // As for a listener: a talker with no secondary talks on channel 0.
    if (!_talking) {
        device->talk(0);
        _talking = true;
    }
    return device->send(dst, maxBytes);
}

void IecBus::reset()
{
    forget_addressing();
    for (IIecDevice* device : _devices) {
        if (device) {
            device->bus_reset();
        }
    }
}

void IecBus::forget_addressing()
{
    _listener = none;
    _talker = none;
    _listening = false;
    _talking = false;
    _opening = false;
    _name.clear();
}

} // namespace fujinet::io
