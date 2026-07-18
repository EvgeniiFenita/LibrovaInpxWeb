#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Domain/BookRepository.hpp"

namespace InpxWebReader::Application {

struct SBookListRequest
{
    std::string TextUtf8;
    InpxWebReader::Domain::SSearchFieldScope SearchFields;
    std::vector<std::string> Languages;
    std::vector<std::string> GenresUtf8;
    std::optional<InpxWebReader::Domain::EBookSort> SortBy = std::nullopt;
    std::optional<InpxWebReader::Domain::ESortDirection> SortDirection = std::nullopt;
    std::size_t Offset = 0;
    std::size_t Limit = 50;
    std::optional<InpxWebReader::Domain::SSearchCursor> Cursor = std::nullopt;
    bool IncludeFacets = true;
    bool IncludeLanguageFacets = true;
    bool IncludeGenreFacets = true;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return Limit > 0;
    }
};

struct SBookListItem
{
    InpxWebReader::Domain::SBookId Id;
    std::string TitleUtf8;
    std::vector<std::string> AuthorsUtf8;
    std::string Language;
    std::optional<std::string> SeriesUtf8 = std::nullopt;
    std::optional<double> SeriesIndex = std::nullopt;
    std::optional<int> Year = std::nullopt;
    std::vector<std::string> TagsUtf8;
    std::vector<std::string> GenresUtf8;
    InpxWebReader::Domain::EBookFormat Format = InpxWebReader::Domain::EBookFormat::Fb2;
    std::optional<std::filesystem::path> CoverPath = std::nullopt;
    std::uintmax_t SizeBytes = 0;
    std::chrono::system_clock::time_point AddedAtUtc = {};
    bool CanDownloadOriginal = true;
    bool CanDownloadAsEpub = false;
    bool IsAvailable = true;
    std::string AvailabilityLabelUtf8;
};

using SCatalogStatistics = InpxWebReader::Domain::IBookQueryRepository::SCatalogStatistics;

struct SBookListResult
{
    std::vector<SBookListItem> Items;
    std::optional<std::uint64_t> TotalCount = std::nullopt;
    std::vector<InpxWebReader::Domain::SFacetItem> AvailableLanguages;
    std::vector<InpxWebReader::Domain::SFacetItem> AvailableGenres;
    std::string CatalogSourceFingerprintUtf8;
    std::string CatalogSnapshotIdUtf8;
    std::optional<InpxWebReader::Domain::SSearchCursor> NextCursor = std::nullopt;
};

struct SBookDetails
{
    InpxWebReader::Domain::SBookId Id;
    std::string TitleUtf8;
    std::vector<std::string> AuthorsUtf8;
    std::string Language;
    std::optional<std::string> SeriesUtf8 = std::nullopt;
    std::optional<double> SeriesIndex = std::nullopt;
    std::optional<std::string> PublisherUtf8 = std::nullopt;
    std::optional<int> Year = std::nullopt;
    std::optional<std::string> Isbn = std::nullopt;
    std::vector<std::string> TagsUtf8;
    std::vector<std::string> GenresUtf8;
    std::optional<std::string> DescriptionUtf8 = std::nullopt;
    std::optional<std::string> Identifier = std::nullopt;
    InpxWebReader::Domain::EBookFormat Format = InpxWebReader::Domain::EBookFormat::Fb2;
    std::optional<std::filesystem::path> CoverPath = std::nullopt;
    std::uintmax_t SizeBytes = 0;
    std::chrono::system_clock::time_point AddedAtUtc = {};
    bool CanDownloadOriginal = true;
    bool CanDownloadAsEpub = false;
    bool IsAvailable = true;
    std::string AvailabilityLabelUtf8;
};

class CInpxCatalogFacade final
{
public:
    explicit CInpxCatalogFacade(const InpxWebReader::Domain::IBookQueryRepository& bookQueryRepository);

    [[nodiscard]] SBookListResult ListBooks(const SBookListRequest& request) const;
    [[nodiscard]] std::optional<SBookDetails> GetBookDetails(InpxWebReader::Domain::SBookId id) const;
    [[nodiscard]] SCatalogStatistics GetCatalogStatistics() const;

private:
    [[nodiscard]] static InpxWebReader::Domain::SSearchQuery ToDomainQuery(const SBookListRequest& request);
    [[nodiscard]] static SBookListItem ToListItem(const InpxWebReader::Domain::SBook& book);
    [[nodiscard]] static SBookDetails ToDetails(const InpxWebReader::Domain::SBook& book);

    const InpxWebReader::Domain::IBookQueryRepository& m_bookQueryRepository;
};

} // namespace InpxWebReader::Application
