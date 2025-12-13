#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::io::bytecodec {

// Bounds-checked reader over a byte span.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : _begin(data), _p(data), _end(data + size) {}

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

    // Convenience: read into string_view (non-owning view into payload)
    bool read_sv(std::string_view& out, std::size_t n) {
        const std::uint8_t* ptr = nullptr;
        if (!read_bytes(ptr, n)) return false;
        out = std::string_view(reinterpret_cast<const char*>(ptr), n);
        return true;
    }

    // Read u16-length-prefixed string into a view (no allocation).
    bool read_lp_u16_string(std::string_view& out) {
        std::uint16_t n = 0;
        if (!read_u16le(n)) return false;
        return read_sv(out, n);
    }

    bool skip(std::size_t n) {
        if (_p + n > _end) return false;
        _p += n;
        return true;
    }

    std::size_t remaining() const { return (std::size_t)(_end - _p); }
    std::size_t pos() const { return (std::size_t)(_p - _begin); }
    bool ok() const { return _p <= _end; }

private:
    const std::uint8_t* _begin{};
    const std::uint8_t* _p{};
    const std::uint8_t* _end{};
};

// -----------------------------
// Writer helpers
// (works with std::string and std::vector<uint8_t>)
// -----------------------------
namespace detail {
inline void push_byte(std::string& out, std::uint8_t b) {
    out.push_back(static_cast<char>(b));
}
inline void push_byte(std::vector<std::uint8_t>& out, std::uint8_t b) {
    out.push_back(b);
}
template <typename Buf>
inline void push_bytes(Buf& out, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) push_byte(out, p[i]);
}
} // namespace detail

template <typename Buf>
inline void write_u8(Buf& out, std::uint8_t v) {
    detail::push_byte(out, v);
}

template <typename Buf>
inline void write_u16le(Buf& out, std::uint16_t v) {
    detail::push_byte(out, (std::uint8_t)(v & 0xFF));
    detail::push_byte(out, (std::uint8_t)((v >> 8) & 0xFF));
}

template <typename Buf>
inline void write_u32le(Buf& out, std::uint32_t v) {
    detail::push_byte(out, (std::uint8_t)(v & 0xFF));
    detail::push_byte(out, (std::uint8_t)((v >> 8) & 0xFF));
    detail::push_byte(out, (std::uint8_t)((v >> 16) & 0xFF));
    detail::push_byte(out, (std::uint8_t)((v >> 24) & 0xFF));
}

template <typename Buf>
inline void write_u64le(Buf& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        detail::push_byte(out, (std::uint8_t)((v >> (8 * i)) & 0xFF));
}

template <typename Buf>
inline void write_bytes(Buf& out, const void* data, std::size_t n) {
    detail::push_bytes(out, data, n);
}

template <typename Buf>
inline void write_sv(Buf& out, std::string_view s) {
    write_bytes(out, s.data(), s.size());
}

template <typename Buf>
inline void write_lp_u16_string(Buf& out, std::string_view s) {
    const auto n = static_cast<std::uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size());
    write_u16le(out, n);
    write_sv(out, std::string_view{s.data(), n});
}

} // namespace fujinet::io::bytecodec


