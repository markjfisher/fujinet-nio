#include <string>
#include <string_view>

namespace fujinet {

std::string_view version()
{
    // TODO: maybe later generate this from git or CMake configure_file
    static constexpr std::string_view v = "0.1.0";
    return v;
}

} // namespace fujinet
