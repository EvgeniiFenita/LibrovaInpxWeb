#include <catch2/catch_test_macros.hpp>

#include <string>

#include "Foundation/FilenamePolicy.hpp"
#include "Foundation/UnicodeConversion.hpp"

TEST_CASE("Download filename policy replaces unsafe header and path bytes", "[foundation][filename]")
{
    REQUIRE(InpxWebReader::Foundation::SanitizeDownloadFilenameBaseUtf8("bad:/name?", 160) == "bad__name_");
    REQUIRE(InpxWebReader::Foundation::SanitizeDownloadFilenameBaseUtf8("quoted\"name", 160) == "quoted_name");
}

TEST_CASE("Filename policy preserves UTF-8 while trimming to a byte limit", "[foundation][filename]")
{
    const std::string value = "Тестовая книга с длинным названием";
    const std::string sanitized = InpxWebReader::Foundation::SanitizeDownloadFilenameBaseUtf8(value, 17);

    REQUIRE(InpxWebReader::Unicode::IsValidUtf8(sanitized));
    REQUIRE(sanitized.size() <= 17);
    REQUIRE_FALSE(sanitized.empty());
}
