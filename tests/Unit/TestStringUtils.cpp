#include <catch2/catch_test_macros.hpp>

#include <string>

#include "Foundation/StringUtils.hpp"

TEST_CASE("ASCII string normalization preserves non-ASCII bytes", "[foundation][unicode]")
{
    const std::string value = "\xD0\x90" "BC.ZIP";

    REQUIRE(InpxWebReader::Foundation::ToLowerAscii(value) == "\xD0\x90" "bc.zip");
    REQUIRE(InpxWebReader::Foundation::EndsWithAsciiInsensitive(value, ".zip"));
    REQUIRE_FALSE(InpxWebReader::Foundation::EndsWithAsciiInsensitive(value, ".inp"));
}
