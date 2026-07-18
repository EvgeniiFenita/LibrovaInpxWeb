#pragma once

#include <span>
#include <string_view>

namespace InpxWebReader::Fb2Parsing {

struct SGenreResolution final
{
    std::string_view DisplayName;
    bool IsKnown = false;
};

class CFb2GenreMapper final
{
public:
    [[nodiscard]] static SGenreResolution ResolveGenre(std::string_view fb2Code) noexcept;

    [[nodiscard]] static std::span<const std::string_view> KnownGenreNames();

    // Returns the human-readable English display name for the given FB2 2.1 genre code.
    // All-whitespace or empty input returns an empty view.
    // Unrecognized non-whitespace codes return a view into fb2Code itself — the caller must
    // ensure fb2Code remains valid for as long as the returned view is used.
    [[nodiscard]] static std::string_view ResolveGenreName(std::string_view fb2Code) noexcept;
};

} // namespace InpxWebReader::Fb2Parsing
