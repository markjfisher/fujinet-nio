#pragma once

#include <string>

namespace fujinet::fs {

struct UriParts {
    std::string scheme;
    std::string authority;
    std::string path;
};

UriParts parse_uri(const std::string& uri);

} // namespace fujinet::fs
