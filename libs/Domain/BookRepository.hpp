#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Domain/Book.hpp"
#include "Domain/SearchQuery.hpp"

namespace InpxWebReader::Domain {

struct SFacetItem
{
    std::string Value;
    std::uint32_t Count = 0;

    [[nodiscard]] bool operator==(const SFacetItem&) const noexcept = default;
};

class IBookQueryRepository
{
public:
    struct SCatalogStatistics
    {
        std::uint64_t BookCount = 0;
        std::uint64_t UnavailableBookCount = 0;
        std::uint64_t InpxSourceSizeBytes = 0;
        std::uint64_t CoverCacheSizeBytes = 0;
        std::uint64_t DatabaseSizeBytes = 0;
        std::uint64_t TotalCatalogSizeBytes = 0;
    };

    struct SSearchPage
    {
        std::vector<SBook> Books;
        std::optional<std::uint64_t> TotalCount = std::nullopt;
        std::vector<SFacetItem> AvailableLanguages;
        std::vector<SFacetItem> AvailableGenres;
        std::string SourceFingerprintUtf8;
        std::string CatalogSnapshotIdUtf8;
        std::optional<SSearchCursor> NextCursor = std::nullopt;
    };

    virtual ~IBookQueryRepository() = default;

    [[nodiscard]] virtual std::vector<SBook> Search(const SSearchQuery& query) const = 0;
    [[nodiscard]] virtual std::optional<SBook> GetById(SBookId id) const = 0;
    [[nodiscard]] virtual std::uint64_t CountSearchResults(const SSearchQuery& query) const = 0;
    [[nodiscard]] virtual std::vector<SFacetItem> ListAvailableLanguages(const SSearchQuery& query) const = 0;
    [[nodiscard]] virtual std::vector<SFacetItem> ListAvailableGenres(const SSearchQuery& query) const = 0;
    [[nodiscard]] virtual SCatalogStatistics GetCatalogStatistics() const = 0;

    [[nodiscard]] virtual SSearchPage SearchPage(
        const SSearchQuery& query,
        const bool includeLanguageFacets,
        const bool includeGenreFacets) const
    {
        auto languageFacetQuery = query;
        languageFacetQuery.Languages.clear();
        auto genreFacetQuery = query;
        genreFacetQuery.GenresUtf8.clear();

        SSearchPage result{
            .Books = Search(query),
            .TotalCount = query.Cursor.has_value()
                ? std::nullopt
                : std::make_optional(CountSearchResults(query))
        };
        if (!query.Cursor.has_value())
        {
            result.AvailableLanguages = includeLanguageFacets
                ? ListAvailableLanguages(languageFacetQuery)
                : std::vector<SFacetItem>{};
            result.AvailableGenres = includeGenreFacets
                ? ListAvailableGenres(genreFacetQuery)
                : std::vector<SFacetItem>{};
        }
        return result;
    }
};

} // namespace InpxWebReader::Domain
