#include "doctest.h"

#include "fujinet/io/list_directory_format.h"

#include <chrono>
#include <string>

using fujinet::fs::FileInfo;
using fujinet::io::format_list_directory_line;

namespace {

FileInfo make_entry(bool is_dir, std::uint64_t size_bytes)
{
    return FileInfo{"", is_dir, size_bytes, std::chrono::system_clock::time_point{}};
}

std::string line_size_field(std::string_view line)
{
    // "d " or "- " then kSizeFieldWidth (7) size chars
    REQUIRE(line.size() >= 9);
    return std::string(line.substr(2, 7));
}

} // namespace

TEST_CASE("format_list_directory_line size uses one decimal for scaled units")
{
    const auto dir_line = format_list_directory_line(make_entry(true, 4096), "aardvark");
    CHECK(line_size_field(dir_line) == "   4.0K");

    const auto small_file = format_list_directory_line(make_entry(false, 5), "ddddd");
    CHECK(line_size_field(small_file) == "     5B");

    const auto ki_file = format_list_directory_line(make_entry(false, 204800), "ctests.ssd");
    CHECK(line_size_field(ki_file) == " 200.0K");
}

TEST_CASE("format_list_directory_line appends slash to directory names")
{
    const auto line = format_list_directory_line(make_entry(true, 4096), "aardvark");
    CHECK(line.find("aardvark/") != std::string::npos);
    CHECK(line.find("aardvark/\n") != std::string::npos);

    const auto already_slash =
        format_list_directory_line(make_entry(true, 4096), "fish/");
    CHECK(already_slash.find("fish/\n") != std::string::npos);
    CHECK(already_slash.find("fish//") == std::string::npos);

    const auto file_line = format_list_directory_line(make_entry(false, 18), "test.txt");
    CHECK(file_line.find("test.txt\n") != std::string::npos);
    CHECK(file_line.find("test.txt/") == std::string::npos);
}

TEST_CASE("format_list_directory_line rolls 1000M to gigabytes")
{
    constexpr std::uint64_t kThousandMiB = 1000ULL * 1024ULL * 1024ULL;
    const auto line = format_list_directory_line(make_entry(false, kThousandMiB), "zeros.bin");
    CHECK(line_size_field(line) == "   1.0G");

    constexpr std::uint64_t kOneGiB = 1024ULL * 1024ULL * 1024ULL;
    const auto exact_gib =
        format_list_directory_line(make_entry(false, kOneGiB), "exact.bin");
    CHECK(line_size_field(exact_gib) == "   1.0G");
}
