#include "fujinet/fs/path_resolver_utils.h"

#include <vector>

namespace fujinet::fs {

std::string fs_join(std::string_view base, std::string_view rel)
{
    if (base.empty()) return std::string(rel);
    if (base.back() == '/') {
        if (!rel.empty() && rel.front() == '/') {
            return std::string(base) + std::string(rel.substr(1));
        }
        return std::string(base) + std::string(rel);
    }
    if (!rel.empty() && rel.front() == '/') {
        return std::string(base) + std::string(rel);
    }
    std::string out(base);
    out.push_back('/');
    out.append(rel.data(), rel.size());
    return out;
}

std::string fs_norm(std::string_view in)
{
    std::vector<std::string_view> parts;
    std::string_view s = in;

    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == '/') ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != '/') ++i;
        if (i == start) break;
        parts.push_back(s.substr(start, i - start));
    }

    std::vector<std::string_view> stack;
    for (std::string_view p : parts) {
        if (p == "." || p.empty()) continue;
        if (p == "..") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        stack.push_back(p);
    }

    std::string out;
    out.push_back('/');
    for (std::size_t k = 0; k < stack.size(); ++k) {
        if (k != 0) out.push_back('/');
        out.append(stack[k].data(), stack[k].size());
    }
    return out;
}

} // namespace fujinet::fs
