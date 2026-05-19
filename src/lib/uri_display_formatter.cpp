#include "fujinet/io/uri_display_formatter.h"

#include "fujinet/fs/uri_parser.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <charconv>
#include <string>

namespace fujinet::io {
namespace {

bool parse_port(std::string_view text, std::uint16_t& out)
{
    if (text.empty()) {
        return false;
    }

    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > 0xFFFFU) {
        return false;
    }

    out = static_cast<std::uint16_t>(value);
    return true;
}

std::string join_scheme_and_authority(std::string_view scheme, std::string_view authority)
{
    std::string out;
    out.reserve(scheme.size() + authority.size() + 3);
    out.append(scheme);
    out.append("://");
    out.append(authority);
    return out;
}

UriDisplayParts format_tnfs_uri(const fujinet::fs::UriParts& parts)
{
    UriDisplayParts display;

    std::string_view authority = parts.authority;
    std::string_view host = authority;
    std::string_view port_text;

    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        std::uint16_t parsed_port = 0;
        const std::string_view candidate = authority.substr(colon + 1);
        if (parse_port(candidate, parsed_port)) {
            host = authority.substr(0, colon);
            if (parsed_port != fujinet::tnfs::DEFAULT_PORT) {
                port_text = candidate;
            }
        }
    }

    std::string authority_display(host);
    if (!port_text.empty()) {
        authority_display.push_back(':');
        authority_display.append(port_text);
    }

    display.summary = join_scheme_and_authority(parts.scheme, authority_display);
    display.detail = parts.path.empty() ? "/" : parts.path;
    return display;
}

UriDisplayParts format_generic_uri(const fujinet::fs::UriParts& parts)
{
    UriDisplayParts display;

    if (!parts.scheme.empty() && !parts.authority.empty()) {
        display.summary = join_scheme_and_authority(parts.scheme, parts.authority);
        display.detail = parts.path.empty() ? "/" : parts.path;
        return display;
    }

    if (!parts.scheme.empty()) {
        display.summary = parts.scheme + ":";
        display.detail = parts.path.empty() ? "/" : parts.path;
        return display;
    }

    display.summary = parts.path.empty() ? "/" : parts.path;
    return display;
}

} // namespace

UriDisplayParts format_uri_for_display(std::string_view uri)
{
    const auto parts = fujinet::fs::parse_uri(std::string(uri));
    if (parts.scheme == "tnfs") {
        return format_tnfs_uri(parts);
    }

    return format_generic_uri(parts);
}

} // namespace fujinet::io
