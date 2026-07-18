#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <filesystem>
#include <string>

#include "Foundation/UnicodeConversion.hpp"

TEST_CASE("Unicode conversion converts Cyrillic paths to UTF-8", "[unicode]")
{
    const std::filesystem::path path = std::filesystem::path(u8"Archives/Автор/Книга.fb2");
    const auto expectedUtf8 = path.generic_u8string();
    const std::string expected(
        reinterpret_cast<const char*>(expectedUtf8.data()),
        expectedUtf8.size());

    REQUIRE(InpxWebReader::Unicode::PathToUtf8(path) == expected);
}

TEST_CASE("Unicode conversion restores Cyrillic paths from UTF-8", "[unicode]")
{
    const auto utf8Bytes = std::filesystem::path(u8"Archives/Автор/Книга.fb2").generic_u8string();
    const std::string utf8Path(
        reinterpret_cast<const char*>(utf8Bytes.data()),
        utf8Bytes.size());

    REQUIRE(InpxWebReader::Unicode::PathFromUtf8(utf8Path) == std::filesystem::path(u8"Archives/Автор/Книга.fb2"));
}

TEST_CASE("Unicode conversion decodes windows-1251 text into UTF-8", "[unicode]")
{
    const std::string cp1251Text("\xCA\xED\xE8\xE3\xE0");
    const auto expectedUtf8Bytes = std::filesystem::path(u8"Книга").generic_u8string();
    const std::string expectedUtf8(
        reinterpret_cast<const char*>(expectedUtf8Bytes.data()),
        expectedUtf8Bytes.size());

    REQUIRE(InpxWebReader::Unicode::CodePageToUtf8(
        cp1251Text,
        1251,
        "cp1251 decode failed") == expectedUtf8);
}

TEST_CASE("Unicode conversion decodes undefined windows-1251 bytes lossily", "[unicode]")
{
    const std::string cp1251Text("A\x98\xC1");
    REQUIRE(InpxWebReader::Unicode::Windows1251ToUtf8Lossy(cp1251Text) == std::string{"A\xEF\xBF\xBD"} + "Б");
}

TEST_CASE("Unicode conversion decodes CP866 text into UTF-8", "[unicode]")
{
    const std::string cp866Text("\xAA\xAD\xA8\xA3\xA0");
    const auto expectedUtf8Bytes = std::filesystem::path(u8"книга").generic_u8string();
    const std::string expectedUtf8(
        reinterpret_cast<const char*>(expectedUtf8Bytes.data()),
        expectedUtf8Bytes.size());

    REQUIRE(InpxWebReader::Unicode::CodePageToUtf8(
        cp866Text,
        866,
        "cp866 decode failed") == expectedUtf8);
}

TEST_CASE("Unicode conversion detects CP1251 and CP866 Cyrillic text", "[unicode]")
{
    const std::string cp1251Text("\xCA\xED\xE8\xE3\xE0");
    const std::string cp866Text("\x8A\xAD\xA8\xA3\xA0");

    REQUIRE(InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
        cp1251Text,
        "legacy decode failed") == "Книга");
    REQUIRE(InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
        cp866Text,
        "legacy decode failed") == "Книга");
}

TEST_CASE("Legacy Cyrillic detection keeps damaged CP1251 text in its source code page", "[unicode]")
{
    const std::string damagedCp1251Text("\xCA\xED\xE8\x98\xE3\xE0");

    REQUIRE(InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
        damagedCp1251Text,
        "legacy decode failed") == std::string{"Кни"} + "\xEF\xBF\xBD" + "га");
}

TEST_CASE("Legacy Cyrillic detection uses word shape to resolve a CP866 score tie", "[unicode]")
{
    const std::string cp866Text("\xE2\xE3\xE2");

    REQUIRE(InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
        cp866Text,
        "legacy decode failed") == "тут");
}

TEST_CASE("Legacy Cyrillic detection rejects genuinely ambiguous input", "[unicode]")
{
    const std::string ambiguousText("\xE2");

    REQUIRE_THROWS_WITH(
        InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
            ambiguousText,
            "legacy decode failed"),
        "legacy decode failed: legacy Cyrillic encoding is ambiguous between Windows-1251 and CP866.");
}

TEST_CASE("Unicode conversion rejects odd UTF-16 LE byte count", "[unicode]")
{
    const std::array<unsigned char, 5> bytes = {0xFF, 0xFE, 0x41, 0x00, 0x42};

    try
    {
        static_cast<void>(InpxWebReader::Unicode::Utf16LeToUtf8(bytes.data(), bytes.size()));
        FAIL("Expected Utf16LeToUtf8 to reject odd byte count.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()} == "Failed to convert UTF-16 LE text to UTF-8: odd byte count.");
    }
}

TEST_CASE("Unicode conversion rejects odd UTF-16 BE byte count", "[unicode]")
{
    const std::array<unsigned char, 5> bytes = {0xFE, 0xFF, 0x00, 0x41, 0x42};

    try
    {
        static_cast<void>(InpxWebReader::Unicode::Utf16BeToUtf8(bytes.data(), bytes.size()));
        FAIL("Expected Utf16BeToUtf8 to reject odd byte count.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()} == "Failed to convert UTF-16 BE text to UTF-8: odd byte count.");
    }
}

TEST_CASE("Unicode conversion rejects odd UTF-16 LE byte count without BOM", "[unicode]")
{
    const std::array<unsigned char, 3> bytes = {0x41, 0x00, 0x42};

    try
    {
        static_cast<void>(InpxWebReader::Unicode::Utf16LeToUtf8(bytes.data(), bytes.size()));
        FAIL("Expected Utf16LeToUtf8 to reject odd byte count without BOM.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()} == "Failed to convert UTF-16 LE text to UTF-8: odd byte count.");
    }
}

TEST_CASE("Unicode conversion rejects odd UTF-16 BE byte count without BOM", "[unicode]")
{
    const std::array<unsigned char, 3> bytes = {0x00, 0x41, 0x42};

    try
    {
        static_cast<void>(InpxWebReader::Unicode::Utf16BeToUtf8(bytes.data(), bytes.size()));
        FAIL("Expected Utf16BeToUtf8 to reject odd byte count without BOM.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()} == "Failed to convert UTF-16 BE text to UTF-8: odd byte count.");
    }
}

TEST_CASE("IsValidUtf8 accepts valid ASCII", "[unicode]")
{
    REQUIRE(InpxWebReader::Unicode::IsValidUtf8("Hello, world!"));
}

TEST_CASE("IsValidUtf8 accepts valid Cyrillic UTF-8", "[unicode]")
{
    // "Привет" encoded as UTF-8
    const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    REQUIRE(InpxWebReader::Unicode::IsValidUtf8(cyrillic));
}

TEST_CASE("IsValidUtf8 accepts empty string", "[unicode]")
{
    REQUIRE(InpxWebReader::Unicode::IsValidUtf8(""));
}

TEST_CASE("IsValidUtf8 rejects raw CP1251 Cyrillic bytes", "[unicode]")
{
    // "Привет" in CP1251 — not valid UTF-8
    const std::string cp1251 = "\xCF\xF0\xE8\xE2\xE5\xF2";
    REQUIRE_FALSE(InpxWebReader::Unicode::IsValidUtf8(cp1251));
}

TEST_CASE("IsValidUtf8 rejects truncated multi-byte sequence", "[unicode]")
{
    // First byte of a 2-byte sequence with no continuation
    const std::string truncated = "\xC3";
    REQUIRE_FALSE(InpxWebReader::Unicode::IsValidUtf8(truncated));
}

TEST_CASE("Unicode conversion trims only trailing incomplete UTF-8", "[unicode]")
{
    std::string truncated = "prefix ";
    truncated.append("\xD0\x98");
    truncated.push_back('\xD0');

    const std::optional<std::string> trimmed = InpxWebReader::Unicode::TryTrimTrailingIncompleteUtf8(truncated);

    REQUIRE(trimmed.has_value());
    REQUIRE(*trimmed == std::string{"prefix \xD0\x98"});
}

TEST_CASE("Unicode conversion does not trim valid or internally malformed UTF-8", "[unicode]")
{
    REQUIRE_FALSE(InpxWebReader::Unicode::TryTrimTrailingIncompleteUtf8("valid").has_value());

    std::string malformed = "prefix ";
    malformed.push_back('\xD0');
    malformed.push_back('x');
    REQUIRE_FALSE(InpxWebReader::Unicode::TryTrimTrailingIncompleteUtf8(malformed).has_value());
}

TEST_CASE("Unicode conversion checkpoints during large iconv passes", "[unicode][cancellation]")
{
    const std::string source(512ull * 1024ull, 'A');
    std::size_t checkpointCount = 0;

    REQUIRE_THROWS_WITH(
        InpxWebReader::Unicode::CodePageToUtf8(
            source,
            1251,
            "large conversion failed",
            [&]() {
                ++checkpointCount;
                if (checkpointCount >= 4)
                {
                    throw std::runtime_error("injected conversion cancellation");
                }
            }),
        "injected conversion cancellation");
    REQUIRE(checkpointCount >= 4);
}
