#pragma once

#include <cstdint>
#include <string>

namespace fujinet::io {

enum class ContentTranslationType : std::uint8_t {
    None = 0,
    Json = 1,
    Xml = 2,
    Rss = 3,
};

constexpr std::uint32_t NETWORK_OPEN_EXT_TRANSLATION = 1u << 0;

struct TranslationConfig {
    ContentTranslationType type{ContentTranslationType::None};
    std::string selector;
    std::uint8_t flags{0};

    [[nodiscard]] bool enabled() const noexcept
    {
        return type != ContentTranslationType::None;
    }
};

[[nodiscard]] constexpr bool is_known_translation_type(ContentTranslationType type) noexcept
{
    switch (type) {
        case ContentTranslationType::None:
        case ContentTranslationType::Json:
        case ContentTranslationType::Xml:
        case ContentTranslationType::Rss:
            return true;
    }

    return false;
}

} // namespace fujinet::io
