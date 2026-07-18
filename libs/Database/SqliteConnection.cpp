#include "Database/SqliteConnection.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include <sqlite3.h>
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Sqlite {
namespace {

std::string BuildErrorMessage(sqlite3* connection, std::string_view prefix)
{
    const char* errorMessage = connection != nullptr ? sqlite3_errmsg(connection) : "unknown sqlite error";
    std::string result = std::string{prefix};
    result.append(": ");
    result.append(errorMessage);
    return result;
}

} // namespace

CSqliteConnection::CSqliteConnection(const std::filesystem::path& databasePath)
{
    sqlite3* rawConnection = nullptr;
    const std::string utf8DatabasePath = InpxWebReader::Unicode::PathToUtf8(databasePath);
    const int openResult = sqlite3_open_v2(
        utf8DatabasePath.c_str(),
        &rawConnection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);

    if (openResult != SQLITE_OK)
    {
        const std::string errorMessage = BuildErrorMessage(rawConnection, "Failed to open sqlite database");

        if (rawConnection != nullptr)
        {
            sqlite3_close(rawConnection);
        }

        throw std::runtime_error(errorMessage);
    }

    m_connection.reset(rawConnection);
    // Enforce foreign-key constraints for every connection opened by InpxWebReader.
    Execute("PRAGMA foreign_keys = ON;");
    Execute("PRAGMA busy_timeout = 5000;");
    Execute("PRAGMA cache_size = -32768;"); // 32 MB page cache
}

void CSqliteConnection::Execute(const std::string_view sql) const
{
    const std::string sqlText{sql};
    char* errorMessage = nullptr;
    const int execResult = sqlite3_exec(m_connection.get(), sqlText.c_str(), nullptr, nullptr, &errorMessage);

    if (execResult != SQLITE_OK)
    {
        std::string fullMessage = "Failed to execute sqlite statement: ";
        fullMessage.append(errorMessage != nullptr ? errorMessage : "unknown sqlite error");
        sqlite3_free(errorMessage);
        throw std::runtime_error(fullMessage);
    }
}

std::int64_t CSqliteConnection::GetLastInsertRowId() const noexcept
{
    return sqlite3_last_insert_rowid(m_connection.get());
}

sqlite3* CSqliteConnection::GetNativeHandle() const noexcept
{
    return m_connection.get();
}

void CSqliteConnection::SConnectionCloser::operator()(sqlite3* connection) const noexcept
{
    if (connection != nullptr)
    {
        sqlite3_close(connection);
    }
}

} // namespace InpxWebReader::Sqlite
