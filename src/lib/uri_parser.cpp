#include "fujinet/fs/uri_parser.h"

namespace fujinet::fs {

UriParts parse_uri(const std::string& uri)
{
    UriParts parts;
    
    // Find scheme
    auto scheme_end = uri.find(':');
    if (scheme_end != std::string::npos) {
        parts.scheme = uri.substr(0, scheme_end);
        
        // Check if there's an authority (//)
        if (scheme_end + 2 < uri.size() && uri[scheme_end + 1] == '/' && uri[scheme_end + 2] == '/') {
            // Parse authority and path
            auto path_start = uri.find('/', scheme_end + 3);
            if (path_start != std::string::npos) {
                parts.authority = uri.substr(scheme_end + 3, path_start - (scheme_end + 3));
                parts.path = uri.substr(path_start);
            } else {
                parts.authority = uri.substr(scheme_end + 3);
                parts.path = "/";
            }
        } else {
            // No authority, just path
            if (scheme_end + 1 < uri.size() && uri[scheme_end + 1] == '/') {
                parts.path = uri.substr(scheme_end + 1);
            } else {
                parts.path = "/" + uri.substr(scheme_end + 1);
            }
        }
    } else {
        // No scheme, treat as relative path or just path
        if (uri.empty() || uri[0] != '/') {
            parts.path = "/" + uri;
        } else {
            parts.path = uri;
        }
    }
    
    return parts;
}

} // namespace fujinet::fs
