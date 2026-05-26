#pragma once

#include <cstdint>

namespace fujinet::io {

enum class RequestContentProfile : std::uint8_t {
    None = 0,
    JsonBody = 1,
    FormBody = 2,
    TextBody = 3,
};

constexpr std::uint32_t NETWORK_OPEN_EXT_CONTENT_PROFILE = 1u << 1;

[[nodiscard]] constexpr bool is_known_content_profile(RequestContentProfile profile) noexcept
{
    switch (profile) {
        case RequestContentProfile::None:
        case RequestContentProfile::JsonBody:
        case RequestContentProfile::FormBody:
        case RequestContentProfile::TextBody:
            return true;
    }

    return false;
}

} // namespace fujinet::io
