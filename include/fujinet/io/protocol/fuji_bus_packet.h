#pragma once

#include <vector>
#include <optional>
#include <string>
#include <memory>
#include <cassert>
#include <cstdint>
#include <type_traits>

#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::io::protocol {
    enum class SlipByte : std::uint8_t {
        End     = 0xC0,
        Escape  = 0xDB,
        EscEnd  = 0xDC,
        EscEsc  = 0xDD,
    };

    // helper to remove noise of the cast
    constexpr std::uint8_t to_byte(SlipByte b) noexcept {
        return static_cast<std::uint8_t>(b);
    }

    using ByteBuffer = std::vector<std::uint8_t>;

    struct PacketParam {
        std::uint32_t value;
        std::uint8_t  size;   // 1, 2, or 4 bytes
    
        template<typename T>
        PacketParam(T v)
            : value(static_cast<std::uint32_t>(v))
            , size(static_cast<std::uint8_t>(sizeof(T)))
        {
            static_assert(
                std::is_same_v<T, std::uint8_t>  ||
                std::is_same_v<T, std::uint16_t> ||
                std::is_same_v<T, std::uint32_t>,
                "Param can only be uint8_t, uint16_t, or uint32_t"
            );
        }
    
        PacketParam(std::uint32_t v, std::uint8_t s)
            : value(v)
            , size(s)
        {
            assert(s == 1 || s == 2 || s == 4);
        }
    };

    class FujiBusPacket
    {
    private:
        WireDeviceId _device{};
        std::uint8_t _command{};
        std::vector<PacketParam> _params;
        std::optional<ByteBuffer> _data;   // raw payload bytes
    
        // Internal helpers now operate on byte buffers
        ByteBuffer decodeSLIP(const ByteBuffer& input) const;
        ByteBuffer encodeSLIP(const ByteBuffer& input) const;
        bool parse(const ByteBuffer& input);
        std::uint8_t calcChecksum(const ByteBuffer& buf) const;
    
        // Variadic constructor helpers for parameters
        void processArg(std::uint8_t v)  { _params.emplace_back(v); }
        void processArg(std::uint16_t v) { _params.emplace_back(v); }
        void processArg(std::uint32_t v) { _params.emplace_back(v); }
    
        // Payload helpers
        void processArg(const ByteBuffer& buf) { _data = buf; }
        void processArg(ByteBuffer&& buf)      { _data = std::move(buf); }
    
        // Convenience: allow passing a std::string payload; it’s treated as raw bytes
        void processArg(const std::string& s) {
            ByteBuffer buf(s.begin(), s.end());
            _data = std::move(buf);
        }
    
    public:
        FujiBusPacket() = default;

        // Builder helpers (explicit, readable at call sites)
        FujiBusPacket& addParamU8(std::uint8_t v)   { _params.emplace_back(v); return *this; }
        FujiBusPacket& addParamU16(std::uint16_t v) { _params.emplace_back(v); return *this; }
        FujiBusPacket& addParamU32(std::uint32_t v) { _params.emplace_back(v); return *this; }

        FujiBusPacket& setData(ByteBuffer data) { _data = std::move(data); return *this; }
        FujiBusPacket& clearData()              { _data.reset(); return *this; }

        template<typename... Args>
        FujiBusPacket(WireDeviceId dev, std::uint8_t cmd, Args&&... args)
            : _device(dev)
            , _command(cmd)
        {
            (processArg(std::forward<Args>(args)), ...);  // fold expression
        }
    
        // Parsing/serialization now explicitly use ByteBuffer
        static std::unique_ptr<FujiBusPacket> fromSerialized(const ByteBuffer& input);
    
        ByteBuffer serialize() const;
    
        // Accessors
        WireDeviceId device() const { return _device; }
        std::uint8_t command() const { return _command; }
    
        std::uint32_t param(unsigned int index) const {
            return _params[index].value;
        }
    
        unsigned int paramCount() const {
            return static_cast<unsigned int>(_params.size());
        }
    
        const std::optional<ByteBuffer>& data() const {
            return _data;
        }
    
        std::optional<std::string> dataAsString() const
        {
            if (!_data) return std::nullopt;
            return std::string(_data->begin(), _data->end());
        }

        bool tryParamU8(unsigned int index, std::uint8_t& out) const
        {
            if (index >= _params.size()) return false;
            const auto& p = _params[index];
            if (p.size != 1) return false;
            out = static_cast<std::uint8_t>(p.value & 0xFF);
            return true;
        }

    };
} // namespace fujinet::io::protocol
