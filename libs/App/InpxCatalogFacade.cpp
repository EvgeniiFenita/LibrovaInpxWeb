#include "App/InpxCatalogFacade.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Application {
namespace {

[[nodiscard]] std::string AvailabilityLabel(const bool isAvailable)
{
    return isAvailable ? std::string{} : std::string{"Unavailable in current INPX source"};
}

[[nodiscard]] long long ElapsedMilliseconds(const std::chrono::steady_clock::time_point startedAt) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
}

[[nodiscard]] std::optional<std::filesystem::path> ToCoverPath(
    const std::optional<std::string>& coverPathUtf8)
{
    if (!coverPathUtf8.has_value() || coverPathUtf8->empty())
    {
        return std::nullopt;
    }
    return InpxWebReader::Unicode::PathFromUtf8(*coverPathUtf8);
}

} // namespace

CInpxCatalogFacade::CInpxCatalogFacade(
    const InpxWebReader::Domain::IBookQueryRepository& bookQueryRepository)
    : m_bookQueryRepository(bookQueryRepository)
{
}

SBookListResult CInpxCatalogFacade::ListBooks(const SBookListRequest& request) const
{
    if (!request.IsValid())
    {
        throw std::invalid_argument("Book list request must use a positive limit.");
    }

    const auto startedAt = std::chrono::steady_clock::now();
    const auto domainQuery = ToDomainQuery(request);
    auto page = m_bookQueryRepository.SearchPage(
        domainQuery,
        request.IncludeFacets && request.IncludeLanguageFacets,
        request.IncludeFacets && request.IncludeGenreFacets);
    SBookListResult result;
    result.TotalCount = page.TotalCount;
    result.AvailableLanguages = std::move(page.AvailableLanguages);
    result.AvailableGenres = std::move(page.AvailableGenres);
    result.CatalogSourceFingerprintUtf8 = std::move(page.SourceFingerprintUtf8);
    result.CatalogSnapshotIdUtf8 = std::move(page.CatalogSnapshotIdUtf8);
    result.NextCursor = std::move(page.NextCursor);

    result.Items.reserve(page.Books.size());
    for (const auto& book : page.Books)
    {
        result.Items.push_back(ToListItem(book));
    }

    InpxWebReader::Logging::InfoIfInitialized(
        "INPX catalog query completed: total={} returned={} continuation={} limit={} elapsed={}ms",
        result.TotalCount.has_value() ? std::to_string(*result.TotalCount) : std::string{"cached"},
        result.Items.size(),
        request.Cursor.has_value(),
        request.Limit,
        ElapsedMilliseconds(startedAt));
    return result;
}

std::optional<SBookDetails> CInpxCatalogFacade::GetBookDetails(const InpxWebReader::Domain::SBookId id) const
{
    if (!id.IsValid())
    {
        throw std::invalid_argument("Book details request must use a valid book id.");
    }

    const auto book = m_bookQueryRepository.GetById(id);
    return book.has_value() ? std::make_optional(ToDetails(*book)) : std::nullopt;
}

SCatalogStatistics CInpxCatalogFacade::GetCatalogStatistics() const
{
    return m_bookQueryRepository.GetCatalogStatistics();
}

InpxWebReader::Domain::SSearchQuery CInpxCatalogFacade::ToDomainQuery(const SBookListRequest& request)
{
    return {
        .TextUtf8 = request.TextUtf8,
        .SearchFields = request.SearchFields,
        .Languages = request.Languages,
        .GenresUtf8 = request.GenresUtf8,
        .SortBy = request.SortBy,
        .SortDirection = request.SortDirection,
        .Offset = request.Offset,
        .Limit = request.Limit,
        .Cursor = request.Cursor
    };
}

SBookListItem CInpxCatalogFacade::ToListItem(const InpxWebReader::Domain::SBook& book)
{
    return {
        .Id = book.Id,
        .TitleUtf8 = book.Metadata.TitleUtf8,
        .AuthorsUtf8 = book.Metadata.AuthorsUtf8,
        .Language = book.Metadata.Language,
        .SeriesUtf8 = book.Metadata.SeriesUtf8,
        .SeriesIndex = book.Metadata.SeriesIndex,
        .Year = book.Metadata.Year,
        .TagsUtf8 = book.Metadata.TagsUtf8,
        .GenresUtf8 = book.Metadata.GenresUtf8,
        .Format = book.File.Format,
        .CoverPath = ToCoverPath(book.CoverPathUtf8),
        .SizeBytes = book.File.SizeBytes,
        .AddedAtUtc = book.AddedAtUtc,
        .IsAvailable = book.IsAvailable,
        .AvailabilityLabelUtf8 = AvailabilityLabel(book.IsAvailable)
    };
}

SBookDetails CInpxCatalogFacade::ToDetails(const InpxWebReader::Domain::SBook& book)
{
    return {
        .Id = book.Id,
        .TitleUtf8 = book.Metadata.TitleUtf8,
        .AuthorsUtf8 = book.Metadata.AuthorsUtf8,
        .Language = book.Metadata.Language,
        .SeriesUtf8 = book.Metadata.SeriesUtf8,
        .SeriesIndex = book.Metadata.SeriesIndex,
        .PublisherUtf8 = book.Metadata.PublisherUtf8,
        .Year = book.Metadata.Year,
        .Isbn = book.Metadata.Isbn,
        .TagsUtf8 = book.Metadata.TagsUtf8,
        .GenresUtf8 = book.Metadata.GenresUtf8,
        .DescriptionUtf8 = book.Metadata.DescriptionUtf8,
        .Identifier = book.Metadata.Identifier,
        .Format = book.File.Format,
        .CoverPath = ToCoverPath(book.CoverPathUtf8),
        .SizeBytes = book.File.SizeBytes,
        .AddedAtUtc = book.AddedAtUtc,
        .IsAvailable = book.IsAvailable,
        .AvailabilityLabelUtf8 = AvailabilityLabel(book.IsAvailable)
    };
}

} // namespace InpxWebReader::Application
