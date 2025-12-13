#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace fujinet::io::fileproto {

// Simple bounds-checked reader over a byte span.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : _p(data), _end(data + size) {}

    bool read_u8(std::uint8_t& out) {
        if (_p + 1 > _end) return false;
        out = *_p++;
        return true;
    }

    bool read_u16le(std::uint16_t& out) {
        if (_p + 2 > _end) return false;
        out = static_cast<std::uint16_t>(_p[0] | (_p[1] << 8));
        _p += 2;
        return true;
    }

    bool read_u32le(std::uint32_t& out) {
        if (_p + 4 > _end) return false;
        out = (std::uint32_t)_p[0]
            | ((std::uint32_t)_p[1] << 8)
            | ((std::uint32_t)_p[2] << 16)
            | ((std::uint32_t)_p[3] << 24);
        _p += 4;
        return true;
    }

    bool read_u64le(std::uint64_t& out) {
        if (_p + 8 > _end) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) out |= (std::uint64_t)_p[i] << (8 * i);
        _p += 8;
        return true;
    }

    bool read_bytes(const std::uint8_t*& ptr, std::size_t n) {
        if (_p + n > _end) return false;
        ptr = _p;
        _p += n;
        return true;
    }

    std::size_t remaining() const { return (std::size_t)(_end - _p); }

private:
    const std::uint8_t* _p{};
    const std::uint8_t* _end{};
};

inline void write_u16le(std::string& out, std::uint16_t v) {
    out.push_back(char(v & 0xFF));
    out.push_back(char((v >> 8) & 0xFF));
}
inline void write_u32le(std::string& out, std::uint32_t v) {
    out.push_back(char(v & 0xFF));
    out.push_back(char((v >> 8) & 0xFF));
    out.push_back(char((v >> 16) & 0xFF));
    out.push_back(char((v >> 24) & 0xFF));
}
inline void write_u64le(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(char((v >> (8 * i)) & 0xFF));
}

} // namespace fujinet::io::fileproto
