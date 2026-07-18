#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Domain/LanguageNormalization.hpp"

TEST_CASE("Language normalization collapses real-world source aliases", "[language-normalization]")
{
    const std::vector<std::pair<std::string_view, std::string_view>> samples{
        {"ru", "ru"},
        {"RU", "ru"},
        {"rus", "ru"},
        {"ru-RU", "ru"},
        {"Russian", "ru"},
        {"русский", "ru"},
        {"ру", "ru"},
        {"ru-petr1708", "ru"},
        {"ru, engl", "ru"},
        {"eng", "en"},
        {"English", "en"},
        {"en-US", "en"},
        {"EN-GB", "en"},
        {"ukr", "uk"},
        {"uk-UA", "uk"},
        {"ua", "uk"},
        {"ukrian", "uk"},
        {"urkain", "uk"},
        {"bul", "bg"},
        {"български", "bg"},
        {"sp", "es"},
        {"fra", "fr"},
        {"deu", "de"},
        {"ita", "it"},
        {"nld", "nl"},
        {"pol", "pl"},
        {"pt-br", "pt"},
        {"cz", "cs"},
        {"dan", "da"},
        {"chinese", "zh"},
        {"jp", "ja"},
        {"in", "id"},
        {"KZ", "kk"},
        {"turkmen", "tk"},
        {"pentjab", "pa"},
        {"und", ""},
        {"", ""}
    };

    for (const auto& [rawLanguage, expectedLanguage] : samples)
    {
        REQUIRE(InpxWebReader::Domain::NormalizeLanguage(rawLanguage) == expectedLanguage);
    }
}

TEST_CASE("Language normalization preserves known uncommon language codes", "[language-normalization]")
{
    REQUIRE(InpxWebReader::Domain::NormalizeLanguage("eo") == "eo");
    REQUIRE(InpxWebReader::Domain::NormalizeLanguage("ang") == "ang");
    REQUIRE(InpxWebReader::Domain::NormalizeLanguage("frm") == "frm");
    REQUIRE(InpxWebReader::Domain::NormalizeLanguage("ia") == "ia");
}
