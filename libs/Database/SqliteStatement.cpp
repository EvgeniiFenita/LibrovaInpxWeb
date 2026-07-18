#include "Database/SqliteStatement.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace InpxWebReader::Sqlite {
namespace {

void ThrowBindFailure(sqlite3* connection, std::string_view operation)
{
    throw std::runtime_error(std::string{"Failed to "} + std::string{operation} + ": " + sqlite3_errmsg(connection));
}

} // namespace

CSqliteStatement::CSqliteStatement(sqlite3* connection, const std::string_view sql)
{
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(
        connection,
        sql.data(),
        static_cast<int>(sql.size()),
        &rawStatement,
        nullptr);

    if (prepareResult != SQLITE_OK)
    {
        throw std::runtime_error(std::string{"Failed to prepare sqlite statement: "} + sqlite3_errmsg(connection));
    }

    m_statement.reset(rawStatement);
}

void CSqliteStatement::BindInt(const int parameterIndex, const int value) const
{
    sqlite3* connection = sqlite3_db_handle(m_statement.get());

    if (sqlite3_bind_int(m_statement.get(), parameterIndex, value) != SQLITE_OK)
    {
        ThrowBindFailure(connection, "bind sqlite int parameter");
    }
}

void CSqliteStatement::BindInt64(const int parameterIndex, const std::int64_t value) const
{
    sqlite3* connection = sqlite3_db_handle(m_statement.get());

    if (sqlite3_bind_int64(m_statement.get(), parameterIndex, value) != SQLITE_OK)
    {
        ThrowBindFailure(connection, "bind sqlite int64 parameter");
    }
}

void CSqliteStatement::BindDouble(const int parameterIndex, const double value) const
{
    sqlite3* connection = sqlite3_db_handle(m_statement.get());

    if (sqlite3_bind_double(m_statement.get(), parameterIndex, value) != SQLITE_OK)
    {
        ThrowBindFailure(connection, "bind sqlite double parameter");
    }
}

void CSqliteStatement::BindText(const int parameterIndex, const std::string_view value) const
{
    sqlite3* connection = sqlite3_db_handle(m_statement.get());

    if (sqlite3_bind_text(m_statement.get(), parameterIndex, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    {
        ThrowBindFailure(connection, "bind sqlite text parameter");
    }
}

void CSqliteStatement::BindNull(const int parameterIndex) const
{
    sqlite3* connection = sqlite3_db_handle(m_statement.get());

    if (sqlite3_bind_null(m_statement.get(), parameterIndex) != SQLITE_OK)
    {
        ThrowBindFailure(connection, "bind sqlite null parameter");
    }
}

bool CSqliteStatement::Step() const
{
    const int stepResult = sqlite3_step(m_statement.get());

    if (stepResult == SQLITE_ROW)
    {
        return true;
    }

    if (stepResult == SQLITE_DONE)
    {
        return false;
    }

    sqlite3* connection = sqlite3_db_handle(m_statement.get());
    throw std::runtime_error(std::string{"Failed to step sqlite statement: "} + sqlite3_errmsg(connection));
}

void CSqliteStatement::Reset() const
{
    sqlite3_reset(m_statement.get());
    sqlite3_clear_bindings(m_statement.get());
}

int CSqliteStatement::GetColumnInt(const int columnIndex) const
{
    return sqlite3_column_int(m_statement.get(), columnIndex);
}

std::int64_t CSqliteStatement::GetColumnInt64(const int columnIndex) const
{
    return sqlite3_column_int64(m_statement.get(), columnIndex);
}

double CSqliteStatement::GetColumnDouble(const int columnIndex) const
{
    return sqlite3_column_double(m_statement.get(), columnIndex);
}

std::string CSqliteStatement::GetColumnText(const int columnIndex) const
{
    const unsigned char* value = sqlite3_column_text(m_statement.get(), columnIndex);

    if (value == nullptr)
    {
        return {};
    }

    const int byteCount = sqlite3_column_bytes(m_statement.get(), columnIndex);
    return std::string{reinterpret_cast<const char*>(value), static_cast<std::size_t>(byteCount)};
}

bool CSqliteStatement::IsColumnNull(const int columnIndex) const noexcept
{
    return sqlite3_column_type(m_statement.get(), columnIndex) == SQLITE_NULL;
}

void CSqliteStatement::SStatementFinalizer::operator()(sqlite3_stmt* statement) const noexcept
{
    if (statement != nullptr)
    {
        sqlite3_finalize(statement);
    }
}

} // namespace InpxWebReader::Sqlite
