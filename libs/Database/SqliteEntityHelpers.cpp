#include "Database/SqliteEntityHelpers.hpp"

#include <format>
#include <stdexcept>
#include <string>

#include "Database/SqliteStatement.hpp"

namespace InpxWebReader::Sqlite {

std::string BuildIdInClause(const std::size_t count)
{
    std::string sql = "(";

    for (std::size_t index = 0; index < count; ++index)
    {
        if (index > 0)
        {
            sql += ", ";
        }

        sql += "?";
    }

    sql += ")";
    return sql;
}

std::int64_t ResolveEntityId(
    const CSqliteConnection& connection,
    const std::string_view tableName,
    const std::string_view normalizedName,
    const std::string_view displayName)
{
    {
        const std::string insertSql = std::format(
            "INSERT INTO {} (normalized_name, display_name) VALUES (?, ?) "
            "ON CONFLICT(normalized_name) DO NOTHING;",
            tableName);

        CSqliteStatement insertStatement(connection.GetNativeHandle(), insertSql);
        insertStatement.BindText(1, normalizedName);
        insertStatement.BindText(2, displayName);
        static_cast<void>(insertStatement.Step());
    }

    const std::string selectSql = std::format(
        "SELECT id FROM {} WHERE normalized_name = ?;", tableName);

    CSqliteStatement selectStatement(connection.GetNativeHandle(), selectSql);
    selectStatement.BindText(1, normalizedName);

    if (!selectStatement.Step())
    {
        throw std::runtime_error(
            std::format("Failed to resolve entity id after insert in table '{}'.", tableName));
    }

    return selectStatement.GetColumnInt64(0);
}

std::vector<std::string> ReadRelatedEntityNames(
    const CSqliteConnection& connection,
    const std::string_view sql,
    const std::int64_t bookId)
{
    CSqliteStatement statement(connection.GetNativeHandle(), sql);
    statement.BindInt64(1, bookId);

    std::vector<std::string> names;

    while (statement.Step())
    {
        names.push_back(statement.GetColumnText(0));
    }

    return names;
}

std::unordered_map<std::int64_t, std::vector<std::string>> ReadRelatedEntityNamesBatch(
    const CSqliteConnection& connection,
    const std::string_view sqlTemplate,
    const std::vector<std::int64_t>& bookIds)
{
    if (bookIds.empty())
    {
        return {};
    }

    std::string inClause = BuildIdInClause(bookIds.size());
    const std::string sql = std::vformat(sqlTemplate, std::make_format_args(inClause));
    CSqliteStatement statement(connection.GetNativeHandle(), sql);

    int parameterIndex = 1;
    for (const std::int64_t bookId : bookIds)
    {
        statement.BindInt64(parameterIndex++, bookId);
    }

    std::unordered_map<std::int64_t, std::vector<std::string>> namesByBookId;

    while (statement.Step())
    {
        namesByBookId[statement.GetColumnInt64(0)].push_back(statement.GetColumnText(1));
    }

    return namesByBookId;
}

} // namespace InpxWebReader::Sqlite
