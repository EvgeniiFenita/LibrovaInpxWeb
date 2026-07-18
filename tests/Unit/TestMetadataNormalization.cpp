#include <catch2/catch_test_macros.hpp>

#include "Domain/MetadataNormalization.hpp"

TEST_CASE("Text normalization trims, lowercases, and collapses whitespace", "[domain][normalization]")
{
    REQUIRE(InpxWebReader::Domain::NormalizeText("  Roadside   Picnic  ") == "roadside picnic");
    REQUIRE(InpxWebReader::Domain::NormalizeText("\tArkady\r\nStrugatsky ") == "arkady strugatsky");
}

TEST_CASE("Text normalization treats cyrillic yo and ye as equivalent", "[domain][normalization]")
{
    REQUIRE(InpxWebReader::Domain::NormalizeText("Ёжик") == InpxWebReader::Domain::NormalizeText("ежик"));
    REQUIRE(InpxWebReader::Domain::NormalizeText("ФЁДОР") == "федор");
    REQUIRE(InpxWebReader::Domain::NormalizeText("Борис") == "борис");
}

TEST_CASE("Text normalization lowercases extended Cyrillic letters used outside Russian", "[domain][normalization]")
{
    REQUIRE(InpxWebReader::Domain::NormalizeText("ІЇЄЎ") == "іїєў");
    REQUIRE(InpxWebReader::Domain::NormalizeText("ПРИВІТ ЇЖАК ЄЎ") == "привіт їжак єў");
}

TEST_CASE("Text normalization folds Unicode whitespace and non-Cyrillic uppercase letters", "[domain][normalization]")
{
    const std::string nonBreakingSpace = "\xC2\xA0";
    const std::string emSpace = "\xE2\x80\x83";

    REQUIRE(
        InpxWebReader::Domain::NormalizeText(std::string{" À"} + nonBreakingSpace + "LA" + emSpace + "RECHERCHE ")
        == "à la recherche");
    REQUIRE(InpxWebReader::Domain::NormalizeText(std::string{"ÉMILE"} + nonBreakingSpace + "ZOLA") == "émile zola");
}

TEST_CASE("ISBN normalization keeps only valid canonical forms", "[domain][normalization]")
{
    REQUIRE(InpxWebReader::Domain::NormalizeIsbn(std::optional<std::string>{"978-5-17-090334-4"}) == "9785170903344");
    REQUIRE(InpxWebReader::Domain::NormalizeIsbn(std::optional<std::string>{"0-306-40615-2"}) == "0306406152");
    REQUIRE_FALSE(InpxWebReader::Domain::NormalizeIsbn(std::optional<std::string>{"978-5-17-118366-5"}).has_value());
    REQUIRE_FALSE(InpxWebReader::Domain::NormalizeIsbn(std::optional<std::string>{"5-93286-159-0"}).has_value());
    REQUIRE_FALSE(InpxWebReader::Domain::NormalizeIsbn(std::optional<std::string>{"invalid-isbn"}).has_value());
}

TEST_CASE("Malformed UTF-8 does not collapse onto valid normalized text", "[domain][normalization]")
{
    const std::string invalidOverlongA{"\xC1\x81", 2};

    REQUIRE(InpxWebReader::Domain::NormalizeText(invalidOverlongA) != InpxWebReader::Domain::NormalizeText("A"));
    REQUIRE(
        InpxWebReader::Domain::NormalizeText(invalidOverlongA)
        != InpxWebReader::Domain::NormalizeText("invalid-utf8:c181"));

}

TEST_CASE("Valid text that starts with the malformed UTF-8 sentinel remains distinct", "[domain][normalization]")
{
    const std::string sentinel = "\xEE\x80\x80";
    const std::string validSentinelText = sentinel + "invalid-utf8:c181";
    const std::string invalidOverlongA{"\xC1\x81", 2};

    REQUIRE(InpxWebReader::Domain::NormalizeText(validSentinelText) != InpxWebReader::Domain::NormalizeText(invalidOverlongA));
}
