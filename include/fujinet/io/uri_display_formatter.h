#pragma once

#include <string>
#include <string_view>

namespace fujinet::io {

struct UriDisplayParts {
    std::string summary;
    std::string detail;
};

UriDisplayParts format_uri_for_display(std::string_view uri);

} // namespace fujinet::io
