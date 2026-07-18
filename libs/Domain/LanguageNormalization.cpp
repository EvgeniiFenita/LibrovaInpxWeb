#include "Domain/LanguageNormalization.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Domain {
namespace {

struct SLanguageAlias
{
    std::string_view Alias;
    std::string_view Canonical;
};

constexpr std::array GLanguageAliases{
    SLanguageAlias{"",           ""},
    SLanguageAlias{"und",        ""},

    SLanguageAlias{"ru",         "ru"},
    SLanguageAlias{"rus",        "ru"},
    SLanguageAlias{"russian",    "ru"},
    SLanguageAlias{"русский",    "ru"},
    SLanguageAlias{"ру",         "ru"},

    SLanguageAlias{"en",         "en"},
    SLanguageAlias{"eng",        "en"},
    SLanguageAlias{"english",    "en"},

    SLanguageAlias{"uk",         "uk"},
    SLanguageAlias{"ukr",        "uk"},
    SLanguageAlias{"ua",         "uk"},
    SLanguageAlias{"ukrain",     "uk"},
    SLanguageAlias{"ukrian",     "uk"},
    SLanguageAlias{"urkain",     "uk"},

    SLanguageAlias{"bg",         "bg"},
    SLanguageAlias{"bul",        "bg"},
    SLanguageAlias{"български",  "bg"},

    SLanguageAlias{"be",         "be"},
    SLanguageAlias{"bel",        "be"},

    SLanguageAlias{"es",         "es"},
    SLanguageAlias{"spa",        "es"},
    SLanguageAlias{"sp",         "es"},

    SLanguageAlias{"fr",         "fr"},
    SLanguageAlias{"fra",        "fr"},
    SLanguageAlias{"fre",        "fr"},

    SLanguageAlias{"de",         "de"},
    SLanguageAlias{"deu",        "de"},
    SLanguageAlias{"ger",        "de"},

    SLanguageAlias{"it",         "it"},
    SLanguageAlias{"ita",        "it"},

    SLanguageAlias{"nl",         "nl"},
    SLanguageAlias{"nld",        "nl"},
    SLanguageAlias{"dut",        "nl"},

    SLanguageAlias{"pl",         "pl"},
    SLanguageAlias{"pol",        "pl"},

    SLanguageAlias{"cs",         "cs"},
    SLanguageAlias{"ces",        "cs"},
    SLanguageAlias{"cze",        "cs"},
    SLanguageAlias{"cz",         "cs"},

    SLanguageAlias{"da",         "da"},
    SLanguageAlias{"dan",        "da"},

    SLanguageAlias{"zh",         "zh"},
    SLanguageAlias{"zho",        "zh"},
    SLanguageAlias{"chi",        "zh"},
    SLanguageAlias{"chinese",    "zh"},

    SLanguageAlias{"ja",         "ja"},
    SLanguageAlias{"jpn",        "ja"},
    SLanguageAlias{"jp",         "ja"},

    SLanguageAlias{"id",         "id"},
    SLanguageAlias{"ind",        "id"},
    SLanguageAlias{"in",         "id"},

    SLanguageAlias{"kk",         "kk"},
    SLanguageAlias{"kaz",        "kk"},
    SLanguageAlias{"kz",         "kk"},

    SLanguageAlias{"pt",         "pt"},
    SLanguageAlias{"por",        "pt"},

    SLanguageAlias{"ro",         "ro"},
    SLanguageAlias{"ron",        "ro"},
    SLanguageAlias{"rum",        "ro"},

    SLanguageAlias{"tk",         "tk"},
    SLanguageAlias{"tuk",        "tk"},
    SLanguageAlias{"turkmen",    "tk"},

    SLanguageAlias{"pa",         "pa"},
    SLanguageAlias{"pan",        "pa"},
    SLanguageAlias{"pentjab",    "pa"},
};

constexpr std::array GKnownPrimaryCodes{
    std::string_view{"ab"},
    std::string_view{"ang"},
    std::string_view{"ar"},
    std::string_view{"as"},
    std::string_view{"az"},
    std::string_view{"ba"},
    std::string_view{"be"},
    std::string_view{"bg"},
    std::string_view{"ca"},
    std::string_view{"cs"},
    std::string_view{"cu"},
    std::string_view{"cv"},
    std::string_view{"da"},
    std::string_view{"de"},
    std::string_view{"el"},
    std::string_view{"en"},
    std::string_view{"eo"},
    std::string_view{"es"},
    std::string_view{"et"},
    std::string_view{"eu"},
    std::string_view{"fa"},
    std::string_view{"fi"},
    std::string_view{"fr"},
    std::string_view{"frm"},
    std::string_view{"ga"},
    std::string_view{"gd"},
    std::string_view{"he"},
    std::string_view{"hi"},
    std::string_view{"hr"},
    std::string_view{"hu"},
    std::string_view{"hy"},
    std::string_view{"ia"},
    std::string_view{"id"},
    std::string_view{"is"},
    std::string_view{"it"},
    std::string_view{"ja"},
    std::string_view{"ka"},
    std::string_view{"kk"},
    std::string_view{"la"},
    std::string_view{"lt"},
    std::string_view{"lv"},
    std::string_view{"mn"},
    std::string_view{"nl"},
    std::string_view{"no"},
    std::string_view{"pa"},
    std::string_view{"pl"},
    std::string_view{"pt"},
    std::string_view{"ro"},
    std::string_view{"ru"},
    std::string_view{"sa"},
    std::string_view{"sh"},
    std::string_view{"sk"},
    std::string_view{"sq"},
    std::string_view{"sr"},
    std::string_view{"sv"},
    std::string_view{"tg"},
    std::string_view{"tk"},
    std::string_view{"tr"},
    std::string_view{"tt"},
    std::string_view{"uk"},
    std::string_view{"uz"},
    std::string_view{"vi"},
    std::string_view{"zh"},
};

[[nodiscard]] bool IsKnownPrimaryCode(const std::string_view value) noexcept
{
    return std::ranges::find(GKnownPrimaryCodes, value) != GKnownPrimaryCodes.end();
}

[[nodiscard]] std::optional<std::string_view> TryResolveAlias(const std::string_view value) noexcept
{
    const auto iterator = std::ranges::find_if(
        GLanguageAliases,
        [value](const SLanguageAlias& alias) {
            return alias.Alias == value;
        });

    if (iterator == GLanguageAliases.end())
    {
        return std::nullopt;
    }

    return iterator->Canonical;
}

void ReplaceAscii(std::string& value, const char from, const char to)
{
    std::ranges::replace(value, from, to);
}

[[nodiscard]] std::string FirstListToken(const std::string_view value)
{
    const std::size_t separator = value.find_first_of(",;/+");
    if (separator == std::string_view::npos)
    {
        return std::string{value};
    }

    return std::string{value.substr(0, separator)};
}

} // namespace

std::string NormalizeLanguage(const std::string_view languageUtf8)
{
    std::optional<std::string> normalized = InpxWebReader::Unicode::TryNormalizeUtf8WhitespaceAndCaseFold(languageUtf8);
    if (!normalized.has_value())
    {
        return std::string{languageUtf8};
    }

    ReplaceAscii(*normalized, '_', '-');
    *normalized = FirstListToken(*normalized);
    normalized = InpxWebReader::Unicode::TryNormalizeUtf8WhitespaceAndCaseFold(*normalized);
    if (!normalized.has_value())
    {
        return std::string{languageUtf8};
    }

    ReplaceAscii(*normalized, '_', '-');

    if (const std::optional<std::string_view> alias = TryResolveAlias(*normalized); alias.has_value())
    {
        return std::string{*alias};
    }

    if (const std::size_t tagSeparator = normalized->find('-'); tagSeparator != std::string::npos)
    {
        std::string primary = normalized->substr(0, tagSeparator);
        if (const std::optional<std::string_view> alias = TryResolveAlias(primary); alias.has_value())
        {
            return std::string{*alias};
        }

        if (IsKnownPrimaryCode(primary))
        {
            return primary;
        }
    }

    if (IsKnownPrimaryCode(*normalized))
    {
        return *normalized;
    }

    return *normalized;
}

} // namespace InpxWebReader::Domain
