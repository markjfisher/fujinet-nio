#pragma once

#include "fujinet/disk/cbm_volume.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/io/iec_bus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fujinet::io {

// A Commodore disk drive on the IEC bus, as far as a program can see one: LOAD, the
// disk directory, sequential reads and the status channel, backed by the image in
// one DiskService slot. docs/iec/drive.md lists what it does and does not do. It
// composes DiskService directly and has no bus lines or sockets, as the Atari's
// disk frontend. Read-only in this version: writes answer WRITE PROTECT ON.
class IecDriveFrontend final : public IIecDevice {
public:
    static constexpr std::uint8_t first_device = 8;
    static constexpr std::uint8_t command_channel = 15;

    // `alwaysPresent`: answers with no disk mounted, as a powered drive does. Drive 8
    // is that way, so LOAD reports the missing disk and not a missing drive; the
    // others appear when something is mounted on them.
    IecDriveFrontend(disk::DiskService& service, std::size_t slot, bool alwaysPresent);

    bool present() const override;
    void open(std::uint8_t channel, const std::uint8_t* name, std::size_t nameLength) override;
    void close(std::uint8_t channel) override;
    void listen(std::uint8_t channel) override;
    std::size_t receive(const std::uint8_t* data, std::size_t length, bool eoi) override;
    void unlisten() override;
    void talk(std::uint8_t channel) override;
    IecReadResult send(std::uint8_t* dst, std::size_t maxBytes) override;
    void untalk() override;
    void bus_reset() override;

    // The status channel's current message, for tests and diagnostics.
    const std::string& status() const noexcept { return _status; }

private:
    struct OpenChannel {
        bool open{false};
        bool streaming{false};                   // reading a file through `reader`
        std::vector<std::uint8_t> data;          // a directory listing, or the file's next bytes
        std::size_t pos{0};
        std::unique_ptr<disk::CbmVolume> volume; // what `reader` reads through
        disk::CbmFileReader reader;
    };

    void set_status(std::uint8_t code, const char* text, std::uint8_t track = 0, std::uint8_t sector = 0);
    void set_status_ok();
    void report(disk::CbmError error);
    void refuse_write();
    bool media_ready(std::uint32_t& sectorCount);
    std::unique_ptr<disk::CbmVolume> make_volume(std::uint32_t sectorCount);

    void open_data_channel(std::uint8_t channel, const std::string& name);
    void open_directory(OpenChannel& target, const std::string& name);
    void open_directory_track(OpenChannel& target);
    void open_file(OpenChannel& target, std::uint8_t channel, const std::string& name);
    void start_reading(OpenChannel& target, std::uint8_t track, std::uint8_t sector);
    void refill(OpenChannel& target);
    void close_all();
    void run_command(const std::string& command);

    disk::DiskService& _service;
    std::size_t _slot;
    bool _alwaysPresent;

    std::array<OpenChannel, command_channel> _channels{};

    int _listenChannel{-1};
    int _talkChannel{-1};
    std::string _command;
    std::uint8_t _statusCode{0};
    std::string _status;
    std::size_t _statusPos{0};
};

} // namespace fujinet::io
