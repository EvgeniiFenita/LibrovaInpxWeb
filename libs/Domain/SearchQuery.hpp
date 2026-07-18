#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace InpxWebReader::Domain {

enum class ESortDirection
{
    Ascending,
    Descending
};

enum class EBookSort
{
    Title,
    Author,
    DateAdded
};

struct SSearchFieldScope
{
    bool Title = true;
    bool Authors = true;
    bool Description = true;

    [[nodiscard]] bool HasAnyField() const noexcept
    {
        return Title || Authors || Description;
    }

    [[nodiscard]] bool operator==(const SSearchFieldScope&) const noexcept = default;
};

struct SSearchCursor
{
    std::string SessionIdUtf8;
    std::string SourceFingerprintUtf8;
    std::string CatalogSnapshotIdUtf8;
    std::size_t Position = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return !SessionIdUtf8.empty()
            && !SourceFingerprintUtf8.empty()
            && !CatalogSnapshotIdUtf8.empty()
            && Position > 0;
    }

    [[nodiscard]] bool operator==(const SSearchCursor&) const noexcept = default;
};

struct SSearchQuery
{
    std::string TextUtf8;
    SSearchFieldScope SearchFields;
    std::vector<std::string> Languages;
    std::vector<std::string> GenresUtf8;
    std::optional<EBookSort> SortBy = std::nullopt;
    std::optional<ESortDirection> SortDirection = std::nullopt;
    std::size_t Offset = 0;
    std::size_t Limit = 50;
    std::optional<SSearchCursor> Cursor = std::nullopt;

    [[nodiscard]] bool HasText() const noexcept
    {
        return !TextUtf8.empty();
    }
};

} // namespace InpxWebReader::Domain
