#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Domain/BookRepository.hpp"
#include "Database/SqliteConnection.hpp"

namespace InpxWebReader::BookDatabase {

class CSqliteBookQueryRepository final : public InpxWebReader::Domain::IBookQueryRepository
{
public:
    struct SSearchSessionState;
    struct SHooks
    {
        std::function<void()> BeforeSearchSessionPublication;
    };

    explicit CSqliteBookQueryRepository(
        std::filesystem::path databasePath,
        std::uint64_t searchSessionMemoryBudgetBytes = 64ull * 1024ull * 1024ull,
        SHooks hooks = {});

    [[nodiscard]] std::vector<InpxWebReader::Domain::SBook> Search(const InpxWebReader::Domain::SSearchQuery& query) const override;
    [[nodiscard]] std::optional<InpxWebReader::Domain::SBook> GetById(InpxWebReader::Domain::SBookId id) const override;
    [[nodiscard]] std::uint64_t CountSearchResults(const InpxWebReader::Domain::SSearchQuery& query) const override;
    [[nodiscard]] std::vector<InpxWebReader::Domain::SFacetItem> ListAvailableLanguages(const InpxWebReader::Domain::SSearchQuery& query) const override;
    [[nodiscard]] std::vector<InpxWebReader::Domain::SFacetItem> ListAvailableGenres(const InpxWebReader::Domain::SSearchQuery& query) const override;
    [[nodiscard]] InpxWebReader::Domain::IBookQueryRepository::SCatalogStatistics GetCatalogStatistics() const override;
    [[nodiscard]] InpxWebReader::Domain::IBookQueryRepository::SSearchPage SearchPage(
        const InpxWebReader::Domain::SSearchQuery& query,
        bool includeLanguageFacets,
        bool includeGenreFacets) const override;

private:
    std::filesystem::path m_databasePath;
    std::shared_ptr<SSearchSessionState> m_searchSessions;
    SHooks m_hooks;
};

} // namespace InpxWebReader::BookDatabase
