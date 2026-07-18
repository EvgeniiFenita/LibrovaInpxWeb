#pragma once

#include <cstdint>
#include "Domain/Book.hpp"
#include "Database/SqliteConnection.hpp"

namespace InpxWebReader::SearchIndex {

class CSearchIndexMaintenance final
{
public:
    static void InsertBook(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::int64_t bookId,
        const InpxWebReader::Domain::SBookMetadata& metadata);

    static void RemoveBook(
        const InpxWebReader::Sqlite::CSqliteConnection& connection,
        std::int64_t bookId,
        const InpxWebReader::Domain::SBookMetadata& metadata);
};

} // namespace InpxWebReader::SearchIndex
