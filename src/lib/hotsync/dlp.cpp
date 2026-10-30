// Derived from palm-sync (https://github.com/jichu4n/palm-sync),
// Copyright Chuan Ji, Apache License 2.0. See docs/hotsync.md.

// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/dlp.h"

#include <algorithm>

namespace fujinet::hotsync {

namespace {

constexpr std::uint8_t kResponseBit = 0x80;
constexpr std::uint8_t kArgShort = 0x80;
constexpr std::uint8_t kArgLong = 0x40;
constexpr std::uint8_t kArgIdMask = 0x3F;

} // namespace

const char* dlp_error_name(DlpError error)
{
    switch (error) {
    case DlpError::None: return "no error";
    case DlpError::System: return "system error";
    case DlpError::IllegalReq: return "unknown function";
    case DlpError::Memory: return "out of memory";
    case DlpError::Param: return "invalid parameter";
    case DlpError::NotFound: return "not found";
    case DlpError::NoneOpen: return "no open databases";
    case DlpError::DatabaseOpen: return "database open elsewhere";
    case DlpError::TooManyOpenDatabases: return "too many open databases";
    case DlpError::AlreadyExists: return "already exists";
    case DlpError::CantOpen: return "cannot open database";
    case DlpError::RecordDeleted: return "record deleted";
    case DlpError::RecordBusy: return "record busy";
    case DlpError::NotSupported: return "not supported";
    case DlpError::ReadOnly: return "read only";
    case DlpError::NotEnoughSpace: return "not enough space";
    case DlpError::LimitExceeded: return "size limit exceeded";
    case DlpError::CancelSync: return "sync cancelled";
    case DlpError::Transport: return "link failure";
    }
    return "unknown error";
}

DlpArgWriter& DlpArgWriter::u8(std::uint8_t v)
{
    _data.push_back(v);
    return *this;
}

DlpArgWriter& DlpArgWriter::u16(std::uint16_t v)
{
    put_be16(_data, v);
    return *this;
}

DlpArgWriter& DlpArgWriter::u32(std::uint32_t v)
{
    put_be32(_data, v);
    return *this;
}

DlpArgWriter& DlpArgWriter::bytes(const std::uint8_t* data, std::size_t len)
{
    _data.insert(_data.end(), data, data + len);
    return *this;
}

DlpArgWriter& DlpArgWriter::cstring(const std::string& s)
{
    _data.insert(_data.end(), s.begin(), s.end());
    _data.push_back(0);
    return *this;
}

DlpArgWriter& DlpArgWriter::type_id(const char id[4])
{
    return bytes(reinterpret_cast<const std::uint8_t*>(id), 4);
}

std::uint8_t DlpArgReader::u8()
{
    return bytes(1)[0];
}

std::uint16_t DlpArgReader::u16()
{
    return get_be16(bytes(2).data());
}

std::uint32_t DlpArgReader::u32()
{
    return get_be32(bytes(4).data());
}

Bytes DlpArgReader::bytes(std::size_t len)
{
    if (remaining() < len) {
        _failed = true;
        _pos = _data.size();
        // Zeros for the fixed-size fields; never trust a corrupt length.
        return Bytes(len <= 4 ? len : 0, 0);
    }
    Bytes out(_data.begin() + static_cast<std::ptrdiff_t>(_pos),
              _data.begin() + static_cast<std::ptrdiff_t>(_pos + len));
    _pos += len;
    return out;
}

Bytes DlpArgReader::rest()
{
    return bytes(remaining());
}

std::string DlpArgReader::cstring()
{
    auto begin = _data.begin() + static_cast<std::ptrdiff_t>(std::min(_pos, _data.size()));
    auto end = std::find(begin, _data.end(), 0);
    std::string s(begin, end);
    _pos += s.size() + (end != _data.end() ? 1 : 0);
    return s;
}

void DlpArgReader::skip(std::size_t len)
{
    bytes(len);
}

DlpRequest& DlpRequest::arg(const DlpArgWriter& writer, std::uint8_t index)
{
    args.push_back({index, writer.data()});
    return *this;
}

Bytes DlpRequest::encode() const
{
    Bytes out{static_cast<std::uint8_t>(func), static_cast<std::uint8_t>(args.size())};
    for (const Arg& a : args) {
        const auto id = static_cast<std::uint8_t>(kDlpArgIdBase + a.index);
        const std::size_t len = a.data.size();
        if (len <= 0xFF) {
            out.push_back(id);
            out.push_back(static_cast<std::uint8_t>(len));
        } else if (len <= 0xFFFF) {
            out.push_back(static_cast<std::uint8_t>(id | kArgShort));
            out.push_back(0);
            put_be16(out, static_cast<std::uint16_t>(len));
        } else {
            out.push_back(static_cast<std::uint8_t>(id | kArgLong));
            out.push_back(0);
            put_be32(out, static_cast<std::uint32_t>(len));
        }
        out.insert(out.end(), a.data.begin(), a.data.end());
    }
    return out;
}

const Bytes& DlpResponse::arg(std::size_t index) const
{
    static const Bytes empty;
    return index < args.size() ? args[index] : empty;
}

bool DlpResponse::decode(DlpFunc expected, const Bytes& message)
{
    DlpArgReader reader(message);
    const std::uint8_t func = reader.u8();
    const std::uint8_t argc = reader.u8();
    const std::uint16_t errorCode = reader.u16();
    if (reader.failed() || func != (static_cast<std::uint8_t>(expected) | kResponseBit)) {
        return false;
    }

    error = static_cast<DlpError>(errorCode);
    args.clear();
    for (std::uint8_t i = 0; i < argc; ++i) {
        const std::uint8_t idByte = reader.u8();
        std::uint32_t len;
        if (idByte & kArgShort) {
            reader.skip(1);
            len = reader.u16();
        } else if (idByte & kArgLong) {
            reader.skip(1);
            len = reader.u32();
        } else {
            len = reader.u8();
        }
        const std::size_t index = static_cast<std::uint8_t>((idByte & kArgIdMask) - (kDlpArgIdBase & kArgIdMask));
        Bytes value = reader.bytes(len);
        if (reader.failed()) {
            return false;
        }
        if (index >= args.size()) {
            args.resize(index + 1);
        }
        args[index] = std::move(value);
    }
    return true;
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
