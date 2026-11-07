#include "fujinet/io/iec_drive_frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fujinet::io {

namespace {

constexpr std::uint8_t channel_load = 0;
constexpr std::uint8_t channel_save = 1;
constexpr std::size_t file_read_chunk = 254;
constexpr std::size_t max_command_bytes = 255;
constexpr char carriage_return = '\r';
constexpr char shifted_space = static_cast<char>(0xA0);
constexpr std::uint8_t directory_track = 18;

// The DOS status messages a program can meet here, in the 1541's own text: its
// message table puts a space before those that begin with a shared word. INPUT#
// strips it, so most programs never see it.
constexpr std::uint8_t status_ok = 0;
constexpr std::uint8_t status_read_error = 20;
constexpr std::uint8_t status_write_protect = 26;
constexpr std::uint8_t status_syntax = 31;
constexpr std::uint8_t status_no_name = 34;
constexpr std::uint8_t status_write_file_open = 60;
constexpr std::uint8_t status_file_not_open = 61;
constexpr std::uint8_t status_file_not_found = 62;
constexpr std::uint8_t status_type_mismatch = 64;
constexpr std::uint8_t status_illegal_ts = 66;
constexpr std::uint8_t status_no_channel = 70;
constexpr std::uint8_t status_power_on = 73;
constexpr std::uint8_t status_not_ready = 74;

const char* const power_on_text = "FUJINET NIO DOS";

// What an OPEN name asks for: [@][drive:]name[,type][,mode]
struct FileName {
    std::string pattern;
    char type{0};   // 'P', 'S', 'U', 'L', 'D' or 0 for none given
    char mode{0};   // 'R', 'W', 'A', 'M' or 0 for none given
    bool replace{false};
};

// Everything up to a colon names the drive.
std::string after_drive_prefix(const std::string& name)
{
    const auto colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

// Up to two comma arguments in any order, of which only the first letter counts:
// ",S,W", ",SEQ,WRITE" and ",W" all work.
FileName parse_file_name(const std::string& raw)
{
    FileName out;
    std::string rest = raw;
    if (!rest.empty() && rest[0] == '@') {
        out.replace = true;
        rest.erase(0, 1);
    }
    rest = after_drive_prefix(rest);

    std::size_t comma = rest.find(',');
    out.pattern = rest.substr(0, comma);
    while (comma != std::string::npos) {
        const std::size_t next = rest.find(',', comma + 1);
        if (comma + 1 < rest.size()) {
            const char c = rest[comma + 1];
            if (c == 'P' || c == 'S' || c == 'U' || c == 'L' || c == 'D') {
                out.type = c;
            } else if (c == 'R' || c == 'W' || c == 'A' || c == 'M') {
                out.mode = c;
            }
        }
        comma = next;
    }

    // A shifted space ends a name on disk, so it ends one asked for too.
    const auto end = out.pattern.find(shifted_space);
    if (end != std::string::npos) {
        out.pattern.erase(end);
    }
    return out;
}

// LOAD and SAVE default to PRG; the data channels take any type when reading.
char effective_type(const FileName& wanted, std::uint8_t channel) noexcept
{
    if (wanted.type != 0) {
        return wanted.type;
    }
    return channel == channel_load ? 'P' : 0;
}

bool readable_type(disk::CbmFileType type) noexcept
{
    return type == disk::CbmFileType::Prg || type == disk::CbmFileType::Seq ||
           type == disk::CbmFileType::Usr;
}

} // namespace

IecDriveFrontend::IecDriveFrontend(disk::DiskService& service, std::size_t slot, bool alwaysPresent)
    : _service(service), _slot(slot), _alwaysPresent(alwaysPresent)
{
    set_status(status_power_on, power_on_text);
}

bool IecDriveFrontend::present() const
{
    if (_alwaysPresent) {
        return true;
    }
    return _service.info(_slot).inserted || _service.get_pending_mount(_slot).has_value();
}

void IecDriveFrontend::set_status(std::uint8_t code, const char* text, std::uint8_t track, std::uint8_t sector)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%02u,%s,%02u,%02u%c",
                  static_cast<unsigned>(code), text,
                  static_cast<unsigned>(track), static_cast<unsigned>(sector), carriage_return);
    _statusCode = code;
    _status = buffer;
    _statusPos = 0;
}

void IecDriveFrontend::set_status_ok()
{
    set_status(status_ok, " OK");
}

void IecDriveFrontend::report(disk::CbmError error)
{
    switch (error) {
        case disk::CbmError::None:
            set_status_ok();
            break;
        case disk::CbmError::NotReady:
            set_status(status_not_ready, "DRIVE NOT READY");
            break;
        case disk::CbmError::ReadError:
            set_status(status_read_error, "READ ERROR");
            break;
        case disk::CbmError::IllegalTrackSector:
            set_status(status_illegal_ts, "ILLEGAL TRACK OR SECTOR");
            break;
    }
}

void IecDriveFrontend::refuse_write()
{
    std::uint32_t ignored = 0;
    if (!media_ready(ignored)) {
        report(disk::CbmError::NotReady);
    } else {
        set_status(status_write_protect, " WRITE PROTECT ON");
    }
}

bool IecDriveFrontend::media_ready(std::uint32_t& sectorCount)
{
    if (!_service.ensure_mounted(_slot).ok()) {
        return false;
    }
    const disk::DiskSlotInfo info = _service.info(_slot);
    const bool cbmDisk = info.type == disk::ImageType::D64 || info.type == disk::ImageType::Prg;
    if (!info.inserted || !cbmDisk) {
        return false;
    }
    sectorCount = info.geometry.sectorCount;
    return true;
}

std::unique_ptr<disk::CbmVolume> IecDriveFrontend::make_volume(std::uint32_t sectorCount)
{
    return std::make_unique<disk::CbmVolume>(
        [this](std::uint32_t lba, std::uint8_t* dst, std::size_t dstBytes) {
            return _service.read_sector(_slot, lba, dst, dstBytes);
        },
        sectorCount);
}

void IecDriveFrontend::open(std::uint8_t channel, const std::uint8_t* name, std::size_t nameLength)
{
    const std::string text(reinterpret_cast<const char*>(name), nameLength);
    if (channel == command_channel) {
        // OPEN 15,8,15,"cmd" with an empty name does not reach the bus; a client's
        // empty name leaves the status alone, as an empty command session does.
        run_command(text);
        return;
    }
    open_data_channel(channel, text);
}

void IecDriveFrontend::open_data_channel(std::uint8_t channel, const std::string& name)
{
    OpenChannel& target = _channels[channel];
    target = OpenChannel{}; // RUN forgets open files without a CLOSE, so this is routine

    if (name.empty()) {
        set_status(status_no_name, "SYNTAX ERROR");
        return;
    }
    if (name[0] == '#') {
        // Direct block access needs a drive buffer, which this device does not model.
        set_status(status_no_channel, "NO CHANNEL");
        return;
    }
    if (name[0] == '$') {
        if (channel == channel_load) {
            open_directory(target, name);
        } else {
            open_directory_track(target);
        }
        return;
    }
    open_file(target, channel, name);
}

void IecDriveFrontend::open_directory(OpenChannel& target, const std::string& name)
{
    std::uint32_t sectorCount = 0;
    if (!media_ready(sectorCount)) {
        report(disk::CbmError::NotReady);
        return;
    }
    const auto volume = make_volume(sectorCount);

    disk::CbmHeader header;
    const disk::CbmError headerError = volume->read_header(header);
    if (headerError != disk::CbmError::None) {
        report(headerError);
        return;
    }
    // A damaged directory lists as far as it goes, and the status says why it stopped.
    std::vector<disk::CbmDirEntry> entries;
    const disk::CbmError listError = volume->read_directory(entries);

    // "$", "$0", "$:PAT*", "$0:PAT*", "$:*=P"
    std::string pattern;
    char typeLetter = 0;
    const auto colon = name.find(':');
    if (colon != std::string::npos) {
        pattern = name.substr(colon + 1);
        const auto equals = pattern.find('=');
        if (equals != std::string::npos) {
            if (equals + 1 < pattern.size()) {
                typeLetter = pattern[equals + 1];
            }
            pattern.erase(equals);
        }
    }

    target.data = disk::cbm_format_directory(header, entries, pattern, typeLetter);
    target.pos = 0;
    target.open = true;
    report(listError);
}

void IecDriveFrontend::open_directory_track(OpenChannel& target)
{
    // Away from channel 0, "$" is not a listing: a program reading the directory
    // itself gets track 18 as a file, from the header sector along its chain.
    std::uint32_t sectorCount = 0;
    if (!media_ready(sectorCount)) {
        report(disk::CbmError::NotReady);
        return;
    }
    target.volume = make_volume(sectorCount);
    start_reading(target, directory_track, 0);
}

void IecDriveFrontend::start_reading(OpenChannel& target, std::uint8_t track, std::uint8_t sector)
{
    target.reader = disk::CbmFileReader(*target.volume, track, sector);
    target.streaming = true;
    target.open = true;
    target.data.clear();
    target.pos = 0;
    set_status_ok();
    refill(target);
}

void IecDriveFrontend::open_file(OpenChannel& target, std::uint8_t channel, const std::string& name)
{
    const FileName wanted = parse_file_name(name);

    // Channel 0 only ever reads and channel 1 only ever writes, whatever the name says.
    const bool wantsWrite = channel == channel_save ||
                            (channel != channel_load && (wanted.mode == 'W' || wanted.mode == 'A'));
    if (wantsWrite) {
        refuse_write();
        return;
    }
    if (wanted.pattern.empty()) {
        set_status(status_no_name, "SYNTAX ERROR");
        return;
    }

    std::uint32_t sectorCount = 0;
    if (!media_ready(sectorCount)) {
        report(disk::CbmError::NotReady);
        return;
    }
    target.volume = make_volume(sectorCount);

    std::vector<disk::CbmDirEntry> entries;
    const disk::CbmError directoryError = target.volume->read_directory(entries);

    // LOAD"*" is the first program on the disk. Any other name, "0:*" included, is the
    // first entry it matches, and then has to be of the right type.
    const bool firstProgram = channel == channel_load && name == "*";
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const disk::CbmDirEntry& e) {
        if (firstProgram) {
            return e.closed && e.type == disk::CbmFileType::Prg;
        }
        return disk::cbm_name_matches(wanted.pattern, e.name);
    });

    if (entry == entries.end()) {
        if (directoryError != disk::CbmError::None) {
            report(directoryError);
        } else {
            set_status(status_file_not_found, " FILE NOT FOUND");
        }
        target = OpenChannel{};
        return;
    }
    if (!readable_type(entry->type) || !disk::cbm_type_matches(effective_type(wanted, channel), entry->type)) {
        set_status(status_type_mismatch, " FILE TYPE MISMATCH");
        target = OpenChannel{};
        return;
    }
    // Mode M is the recovery read: the one way to read a file never closed.
    if (!entry->closed && wanted.mode != 'M') {
        set_status(status_write_file_open, " WRITE FILE OPEN");
        target = OpenChannel{};
        return;
    }
    start_reading(target, entry->track, entry->sector);
}

void IecDriveFrontend::refill(OpenChannel& target)
{
    // Keep at least one byte in hand until the file ends, so the end-of-file signal
    // can go with the last byte: an IEC talker gives EOI before sending a byte.
    target.data.resize(file_read_chunk);
    bool last = false;
    const std::size_t got = target.reader.read(target.data.data(), target.data.size(), last);
    target.data.resize(got);
    target.pos = 0;
    if (last) {
        target.streaming = false;
        if (target.reader.error() != disk::CbmError::None) {
            report(target.reader.error());
        }
    }
}

void IecDriveFrontend::close_all()
{
    for (auto& channel : _channels) {
        channel = OpenChannel{};
    }
}

void IecDriveFrontend::close(std::uint8_t channel)
{
    if (channel == command_channel) {
        close_all(); // closing the command channel closes everything, as on a 1541
        return;
    }
    _channels[channel] = OpenChannel{};
}

void IecDriveFrontend::listen(std::uint8_t channel)
{
    _listenChannel = channel;
    if (channel == command_channel) {
        _command.clear();
    }
}

std::size_t IecDriveFrontend::receive(const std::uint8_t* data, std::size_t length, bool)
{
    if (_listenChannel != command_channel) {
        return 0; // no channel here takes data: its OPEN already answered WRITE PROTECT ON
    }
    const std::size_t room = max_command_bytes - std::min(max_command_bytes, _command.size());
    _command.append(reinterpret_cast<const char*>(data), std::min(room, length));
    return length; // a command too long is cut short, never refused
}

void IecDriveFrontend::unlisten()
{
    if (_listenChannel == command_channel && !_command.empty()) {
        run_command(_command);
        _command.clear();
    }
    _listenChannel = -1;
}

void IecDriveFrontend::run_command(const std::string& raw)
{
    std::string command = raw;
    if (!command.empty() && command.back() == carriage_return) {
        command.pop_back(); // PRINT# ends what it sends with a CR
    }
    if (command.empty()) {
        return;
    }
    switch (command[0]) {
        case 'I': // initialize: nothing is cached to forget
            set_status_ok();
            return;
        case 'U': {
            // U9/UI and U:/UJ reset the drive; the digit form and the letter form share
            // a low nibble. The block commands (U1, U2) are not modelled.
            const auto which = command.size() > 1 ? static_cast<std::uint8_t>(command[1]) & 0x0F : 0;
            if (which == 0x09 || which == 0x0A) {
                bus_reset();
                return;
            }
            if (which == 0x01 || which == 0x02) {
                // U1/U2 need a direct-access channel, which this drive never opens.
                set_status(status_no_channel, "NO CHANNEL");
                return;
            }
            break;
        }
        case 'B': // B-R, B-W, B-P, B-A, B-F: block access, as U1/U2
            set_status(status_no_channel, "NO CHANNEL");
            return;
        case 'S': // scratch
        case 'V': // validate
        case 'N': // new
        case 'C': // copy
        case 'R': // rename
            refuse_write();
            return;
        default:
            break;
    }
    set_status(status_syntax, "SYNTAX ERROR");
}

void IecDriveFrontend::talk(std::uint8_t channel)
{
    _talkChannel = channel;
}

IecReadResult IecDriveFrontend::send(std::uint8_t* dst, std::size_t maxBytes)
{
    if (_talkChannel == command_channel) {
        // The position survives UNTALK: GET# reads a byte per TALK. Once read to its
        // end the message is gone and the channel reports OK, as a drive's does.
        const std::size_t left = _status.size() - _statusPos;
        const std::size_t n = std::min(left, maxBytes);
        std::memcpy(dst, _status.data() + _statusPos, n);
        _statusPos += n;
        const bool done = _statusPos >= _status.size();
        if (done) {
            set_status_ok();
        }
        return IecReadResult{n, done};
    }

    if (_talkChannel < 0 || static_cast<std::size_t>(_talkChannel) >= _channels.size()) {
        return IecReadResult{0, true};
    }
    OpenChannel& channel = _channels[static_cast<std::size_t>(_talkChannel)];
    if (!channel.open) {
        // A failed OPEN has already said why; otherwise nothing was ever opened here.
        if (_statusCode == status_ok) {
            set_status(status_file_not_open, " FILE NOT OPEN");
        }
        return IecReadResult{0, true};
    }

    std::size_t copied = 0;
    while (copied < maxBytes) {
        if (channel.pos >= channel.data.size()) {
            if (!channel.streaming) {
                break;
            }
            refill(channel);
            continue;
        }
        const std::size_t n = std::min(maxBytes - copied, channel.data.size() - channel.pos);
        std::memcpy(dst + copied, channel.data.data() + channel.pos, n);
        copied += n;
        channel.pos += n;
    }

    // What is left to give decides EOI, so it goes with the final byte. A buffer
    // just emptied may still be followed by more of the file.
    if (channel.pos >= channel.data.size() && channel.streaming) {
        refill(channel);
    }
    const bool done = channel.pos >= channel.data.size() && !channel.streaming;
    return IecReadResult{copied, done};
}

void IecDriveFrontend::untalk()
{
    _talkChannel = -1;
}

void IecDriveFrontend::bus_reset()
{
    close_all();
    _listenChannel = -1;
    _talkChannel = -1;
    _command.clear();
    set_status(status_power_on, power_on_text);
}

} // namespace fujinet::io
