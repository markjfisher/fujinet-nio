#include "doctest.h"

#include "fujinet/io/uri_display_formatter.h"

using fujinet::io::format_uri_for_display;

TEST_CASE("format_uri_for_display elides default tnfs port")
{
    const auto display = format_uri_for_display("tnfs://192.168.1.101:16384/bbc/simple.ssd");

    CHECK(display.summary == "tnfs://192.168.1.101");
    CHECK(display.detail == "/bbc/simple.ssd");
}

TEST_CASE("format_uri_for_display keeps non-default tnfs port")
{
    const auto display = format_uri_for_display("tnfs://192.168.1.101:16385/bbc/simple.ssd");

    CHECK(display.summary == "tnfs://192.168.1.101:16385");
    CHECK(display.detail == "/bbc/simple.ssd");
}

TEST_CASE("format_uri_for_display splits local filesystem uri")
{
    const auto display = format_uri_for_display("sd0:/games/elite.atr");

    CHECK(display.summary == "sd0:");
    CHECK(display.detail == "/games/elite.atr");
}

TEST_CASE("format_uri_for_display leaves plain path as summary")
{
    const auto display = format_uri_for_display("/games/elite.atr");

    CHECK(display.summary == "/games/elite.atr");
    CHECK(display.detail.empty());
}
