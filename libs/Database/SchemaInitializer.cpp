#include "Database/SchemaInitializer.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "Database/DatabaseSchema.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/StringUtils.hpp"

namespace InpxWebReader::DatabaseRuntime {
namespace {

int ReadUserVersionValue(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), "PRAGMA user_version;");

    if (!statement.Step())
    {
        return 0;
    }

    return statement.GetColumnInt(0);
}

bool HasUserSchemaObjects(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM sqlite_master WHERE name NOT GLOB 'sqlite_*';");

    if (!statement.Step())
    {
        return false;
    }

    return statement.GetColumnInt(0) > 0;
}

std::optional<std::string> ToOwnedOptionalString(const std::optional<std::string_view> value)
{
    if (!value.has_value())
    {
        return std::nullopt;
    }

    return std::string{*value};
}

std::string FormatOptionalSqlValue(const std::optional<std::string>& value)
{
    return value.has_value() ? "'" + *value + "'" : "<null>";
}

struct SObservedTableColumn
{
    std::string Name;
    std::string DeclaredType;
    bool NotNull = false;
    int PrimaryKeyOrdinal = 0;
    std::optional<std::string> DefaultValue = std::nullopt;
};

struct SObservedForeignKey
{
    std::string FromColumn;
    std::string ReferencedTable;
    std::string ReferencedColumn;
    std::string OnUpdateAction;
    std::string OnDeleteAction;
};

struct SObservedSchemaObject
{
    std::string Type;
    std::string Name;
    std::string TableName;
    std::string Sql;
};

bool ForeignKeyLess(const SObservedForeignKey& left, const SObservedForeignKey& right)
{
    return std::tie(left.FromColumn, left.ReferencedTable, left.ReferencedColumn, left.OnUpdateAction, left.OnDeleteAction)
        < std::tie(right.FromColumn, right.ReferencedTable, right.ReferencedColumn, right.OnUpdateAction, right.OnDeleteAction);
}

std::vector<SObservedTableColumn> ReadTableShape(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view tableName)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        std::format("PRAGMA table_info({});", tableName));

    std::vector<SObservedTableColumn> columns;
    while (statement.Step())
    {
        columns.push_back({
            .Name = statement.GetColumnText(1),
            .DeclaredType = statement.GetColumnText(2),
            .NotNull = statement.GetColumnInt(3) != 0,
            .PrimaryKeyOrdinal = statement.GetColumnInt(5),
            .DefaultValue = statement.IsColumnNull(4)
                ? std::nullopt
                : std::make_optional(statement.GetColumnText(4))
        });
    }

    return columns;
}

std::vector<SObservedForeignKey> ReadForeignKeys(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view tableName)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        std::format("PRAGMA foreign_key_list({});", tableName));

    std::vector<SObservedForeignKey> foreignKeys;
    while (statement.Step())
    {
        foreignKeys.push_back({
            .FromColumn = statement.GetColumnText(3),
            .ReferencedTable = statement.GetColumnText(2),
            .ReferencedColumn = statement.GetColumnText(4),
            .OnUpdateAction = statement.GetColumnText(5),
            .OnDeleteAction = statement.GetColumnText(6)
        });
    }

    return foreignKeys;
}

std::vector<std::string> ReadIndexColumns(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view indexName)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        std::format("PRAGMA index_info({});", indexName));

    std::vector<std::string> columns;
    while (statement.Step())
    {
        columns.push_back(statement.GetColumnText(2));
    }

    return columns;
}

std::string ReadSchemaObjectSql(
    const InpxWebReader::Sqlite::CSqliteConnection& connection,
    const std::string_view objectName)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT sql FROM sqlite_master WHERE name = ?;");
    statement.BindText(1, objectName);

    if (!statement.Step() || statement.IsColumnNull(0))
    {
        return {};
    }

    return statement.GetColumnText(0);
}

[[nodiscard]] std::string NormalizeSchemaSql(const std::string_view sql)
{
    std::string normalized;
    normalized.reserve(sql.size());
    char quote = '\0';
    for (std::size_t index = 0; index < sql.size(); ++index)
    {
        const char current = sql[index];
        if (quote != '\0')
        {
            normalized.push_back(current);
            if (quote == ']' && current == ']')
            {
                quote = '\0';
            }
            else if (quote != ']' && current == quote)
            {
                if (index + 1 < sql.size() && sql[index + 1] == quote)
                {
                    normalized.push_back(sql[++index]);
                }
                else
                {
                    quote = '\0';
                }
            }
            continue;
        }

        if (current == '-' && index + 1 < sql.size() && sql[index + 1] == '-')
        {
            index += 2;
            while (index < sql.size() && sql[index] != '\n' && sql[index] != '\r')
            {
                ++index;
            }
            continue;
        }
        if (current == '/' && index + 1 < sql.size() && sql[index + 1] == '*')
        {
            index += 2;
            while (index + 1 < sql.size() && (sql[index] != '*' || sql[index + 1] != '/'))
            {
                ++index;
            }
            if (index + 1 < sql.size())
            {
                ++index;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(current)) != 0 || current == ';')
        {
            continue;
        }
        if (current == '\'' || current == '"' || current == '`' || current == '[')
        {
            quote = current == '[' ? ']' : current;
            normalized.push_back(current);
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(current))));
    }
    return normalized;
}

[[nodiscard]] std::map<std::string, SObservedSchemaObject> ReadSchemaObjects(
    const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT type, name, tbl_name, sql FROM sqlite_master "
        "WHERE name NOT GLOB 'sqlite_*' ORDER BY name;");
    std::map<std::string, SObservedSchemaObject> result;
    while (statement.Step())
    {
        SObservedSchemaObject object{
            .Type = statement.GetColumnText(0),
            .Name = statement.GetColumnText(1),
            .TableName = statement.GetColumnText(2),
            .Sql = statement.IsColumnNull(3) ? std::string{} : statement.GetColumnText(3)
        };
        result.emplace(object.Name, std::move(object));
    }
    return result;
}

[[nodiscard]] bool IsFtsShadowObject(const std::string_view name) noexcept
{
    return name.starts_with("search_index_");
}

void ValidateExactSchemaObjects(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteConnection expectedConnection(std::filesystem::path{":memory:"});
    expectedConnection.Execute(InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCreateSchemaScript());
    const auto expectedObjects = ReadSchemaObjects(expectedConnection);
    const auto observedObjects = ReadSchemaObjects(connection);

    for (const auto& [name, observed] : observedObjects)
    {
        const auto expected = expectedObjects.find(name);
        if (expected == expectedObjects.end())
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: unexpected {} '{}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    observed.Type,
                    name));
        }
    }

    for (const auto& [name, expected] : expectedObjects)
    {
        const auto observed = observedObjects.find(name);
        if (observed == observedObjects.end())
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: missing expected {} '{}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    expected.Type,
                    name));
        }
        if (observed->second.Type != expected.Type || observed->second.TableName != expected.TableName)
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: schema object '{}' has an unexpected type or owner.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    name));
        }
        if (!IsFtsShadowObject(name)
            && NormalizeSchemaSql(observed->second.Sql) != NormalizeSchemaSql(expected.Sql))
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: schema object '{}' does not match the exact current definition.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    name));
        }
    }
}

void ValidateRequiredSchemaObjects(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type = ? AND name = ?;");

    for (const auto& object : InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredObjects())
    {
        statement.Reset();
        statement.BindText(1, object.Type);
        statement.BindText(2, object.Name);

        if (statement.Step() && statement.GetColumnInt(0) > 0)
        {
            continue;
        }

        throw std::runtime_error(
            std::format(
                "Database schema version {} is damaged: missing required {} '{}'.",
                InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                object.Type,
                object.Name));
    }
}

void ValidateRequiredTableShapes(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    for (const auto& tableShape : InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredTableShapes())
    {
        const auto observedColumns = ReadTableShape(connection, tableShape.Name);
        const std::size_t sharedColumnCount = (std::min)(observedColumns.size(), tableShape.Columns.size());
        for (std::size_t index = 0; index < sharedColumnCount; ++index)
        {
            const auto& expectedColumn = tableShape.Columns[index];
            const auto& observedColumn = observedColumns[index];
            if (observedColumn.Name != expectedColumn.Name || observedColumn.DeclaredType != expectedColumn.DeclaredType)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' column {} expected '{} {}' but found '{} {}'.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        index,
                        expectedColumn.Name,
                        expectedColumn.DeclaredType,
                        observedColumn.Name,
                        observedColumn.DeclaredType));
            }

            if (observedColumn.NotNull != expectedColumn.NotNull)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' column '{}' expected NOT NULL = {} but found {}.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        expectedColumn.Name,
                        expectedColumn.NotNull,
                        observedColumn.NotNull));
            }

            if (observedColumn.PrimaryKeyOrdinal != expectedColumn.PrimaryKeyOrdinal)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' column '{}' expected primary-key ordinal {} but found {}.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        expectedColumn.Name,
                        expectedColumn.PrimaryKeyOrdinal,
                        observedColumn.PrimaryKeyOrdinal));
            }

            const auto expectedDefaultValue = ToOwnedOptionalString(expectedColumn.DefaultValue);
            if (observedColumn.DefaultValue != expectedDefaultValue)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' column '{}' expected default {} but found {}.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        expectedColumn.Name,
                        FormatOptionalSqlValue(expectedDefaultValue),
                        FormatOptionalSqlValue(observedColumn.DefaultValue)));
            }
        }

        if (observedColumns.size() < tableShape.Columns.size())
        {
            const auto& missingColumn = tableShape.Columns[observedColumns.size()];
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: table '{}' is missing expected column '{} {}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    tableShape.Name,
                    missingColumn.Name,
                    missingColumn.DeclaredType));
        }

        if (observedColumns.size() > tableShape.Columns.size())
        {
            const auto& unexpectedColumn = observedColumns[tableShape.Columns.size()];
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: table '{}' has unexpected extra column '{} {}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    tableShape.Name,
                    unexpectedColumn.Name,
                    unexpectedColumn.DeclaredType));
        }

        auto observedForeignKeys = ReadForeignKeys(connection, tableShape.Name);
        if (observedForeignKeys.size() != tableShape.ForeignKeys.size())
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: table '{}' has unexpected foreign key count.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    tableShape.Name));
        }

        std::ranges::sort(observedForeignKeys, ForeignKeyLess);
        std::vector<SObservedForeignKey> expectedForeignKeys;
        expectedForeignKeys.reserve(tableShape.ForeignKeys.size());
        for (const auto& expectedForeignKey : tableShape.ForeignKeys)
        {
            expectedForeignKeys.push_back({
                .FromColumn = std::string{expectedForeignKey.FromColumn},
                .ReferencedTable = std::string{expectedForeignKey.ReferencedTable},
                .ReferencedColumn = std::string{expectedForeignKey.ReferencedColumn},
                .OnUpdateAction = std::string{expectedForeignKey.OnUpdateAction},
                .OnDeleteAction = std::string{expectedForeignKey.OnDeleteAction}
            });
        }
        std::ranges::sort(expectedForeignKeys, ForeignKeyLess);

        for (std::size_t index = 0; index < expectedForeignKeys.size(); ++index)
        {
            const auto& expectedForeignKey = expectedForeignKeys[index];
            const auto& observedForeignKey = observedForeignKeys[index];
            if (observedForeignKey.FromColumn != expectedForeignKey.FromColumn
                || observedForeignKey.ReferencedTable != expectedForeignKey.ReferencedTable
                || observedForeignKey.ReferencedColumn != expectedForeignKey.ReferencedColumn
                || observedForeignKey.OnUpdateAction != expectedForeignKey.OnUpdateAction
                || observedForeignKey.OnDeleteAction != expectedForeignKey.OnDeleteAction)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' foreign key {} expected '{} -> {}.{}' with ON UPDATE {} and ON DELETE {} but found '{} -> {}.{}' with ON UPDATE {} and ON DELETE {}.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        index,
                        expectedForeignKey.FromColumn,
                        expectedForeignKey.ReferencedTable,
                        expectedForeignKey.ReferencedColumn,
                        expectedForeignKey.OnUpdateAction,
                        expectedForeignKey.OnDeleteAction,
                        observedForeignKey.FromColumn,
                        observedForeignKey.ReferencedTable,
                        observedForeignKey.ReferencedColumn,
                        observedForeignKey.OnUpdateAction,
                        observedForeignKey.OnDeleteAction));
            }
        }

        const std::string createSql = Foundation::ToLowerAscii(ReadSchemaObjectSql(connection, tableShape.Name));
        for (const auto requiredToken : tableShape.RequiredSqlTokens)
        {
            if (createSql.find(requiredToken) == std::string::npos)
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: table '{}' is missing required SQL token '{}'.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        tableShape.Name,
                        requiredToken));
            }
        }
    }
}

void ValidateRequiredIndexShapes(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    for (const auto& indexShape : InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredIndexShapes())
    {
        const auto observedColumns = ReadIndexColumns(connection, indexShape.Name);
        if (observedColumns.empty())
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: missing required index '{}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    indexShape.Name));
        }

        if (observedColumns.size() != indexShape.Columns.size())
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: index '{}' has unexpected column count.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    indexShape.Name));
        }

        for (std::size_t index = 0; index < indexShape.Columns.size(); ++index)
        {
            if (observedColumns[index] != indexShape.Columns[index])
            {
                throw std::runtime_error(
                    std::format(
                        "Database schema version {} is damaged: index '{}' expected column '{}' at position {} but found '{}'.",
                        InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                        indexShape.Name,
                        indexShape.Columns[index],
                        index,
                        observedColumns[index]));
            }
        }
    }
}

void ValidateRequiredSearchIndexShape(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    const auto& searchIndexShape = InpxWebReader::DatabaseSchema::CDatabaseSchema::GetRequiredSearchIndexShape();
    const auto observedColumns = ReadTableShape(connection, searchIndexShape.Name);
    if (observedColumns.size() != searchIndexShape.Columns.size())
    {
        throw std::runtime_error(
            std::format(
                "Database schema version {} is damaged: FTS table '{}' has unexpected column count.",
                InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                searchIndexShape.Name));
    }

    for (std::size_t index = 0; index < searchIndexShape.Columns.size(); ++index)
    {
        if (observedColumns[index].Name != searchIndexShape.Columns[index])
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: FTS table '{}' expected column '{}' at position {} but found '{}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    searchIndexShape.Name,
                    searchIndexShape.Columns[index],
                    index,
                    observedColumns[index].Name));
        }
    }

    const std::string createSql = Foundation::ToLowerAscii(ReadSchemaObjectSql(connection, searchIndexShape.Name));
    for (const auto requiredToken : searchIndexShape.RequiredSqlTokens)
    {
        if (createSql.find(requiredToken) == std::string::npos)
        {
            throw std::runtime_error(
                std::format(
                    "Database schema version {} is damaged: FTS table '{}' is missing required SQL token '{}'.",
                    InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion(),
                    searchIndexShape.Name,
                    requiredToken));
        }
    }
}

void ValidateCatalogRelations(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement foreignKeyCheck(
        connection.GetNativeHandle(),
        "PRAGMA foreign_key_check;");
    if (foreignKeyCheck.Step())
    {
        throw std::runtime_error(
            std::format(
                "Database schema version {} is damaged: persisted rows violate a foreign key.",
                InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion()));
    }

    InpxWebReader::Sqlite::CSqliteStatement missingLocatorCheck(
        connection.GetNativeHandle(),
        "SELECT 1 FROM inpx_books b "
        "LEFT JOIN inpx_book_locations l ON l.book_id = b.id "
        "WHERE l.book_id IS NULL LIMIT 1;");
    if (missingLocatorCheck.Step())
    {
        throw std::runtime_error(
            std::format(
                "Database schema version {} is damaged: every INPX book must have an INPX locator.",
                InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion()));
    }
}

void ValidateDatabaseShape(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    ValidateRequiredSchemaObjects(connection);
    ValidateRequiredTableShapes(connection);
    ValidateRequiredIndexShapes(connection);
    ValidateRequiredSearchIndexShape(connection);
    ValidateCatalogRelations(connection);
    ValidateExactSchemaObjects(connection);
}

void ValidateCatalogSingletons(const InpxWebReader::Sqlite::CSqliteConnection& connection)
{
    InpxWebReader::Sqlite::CSqliteStatement source(
        connection.GetNativeHandle(),
        "SELECT COUNT(*), COALESCE(MIN(id), 0), COALESCE(MAX(id), 0), "
        "COALESCE(SUM(CASE WHEN display_name = '' OR source_fingerprint = '' THEN 1 ELSE 0 END), 0) "
        "FROM inpx_sources;");
    if (!source.Step()
        || source.GetColumnInt64(0) != 1
        || source.GetColumnInt64(1) != 1
        || source.GetColumnInt64(2) != 1
        || source.GetColumnInt64(3) != 0)
    {
        throw std::runtime_error(
            "Database schema version 1 is damaged: the INPX source singleton is missing or invalid. "
            "Delete the cache database so it can be recreated.");
    }

    InpxWebReader::Sqlite::CSqliteStatement statistics(
        connection.GetNativeHandle(),
        "SELECT COUNT(*), COALESCE(MIN(singleton), 0), COALESCE(MAX(singleton), 0) "
        "FROM catalog_statistics;");
    if (!statistics.Step()
        || statistics.GetColumnInt64(0) != 1
        || statistics.GetColumnInt64(1) != 1
        || statistics.GetColumnInt64(2) != 1)
    {
        throw std::runtime_error(
            "Database schema version 1 is damaged: the catalog statistics singleton is missing or invalid. "
            "Delete the cache database so it can be recreated.");
    }
}

void InitializeDatabase(
    const std::filesystem::path& databasePath,
    const CSchemaInitializer::TCatalogSeed* seedNewCatalog,
    const bool requireCatalogSingletons)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);

    const int currentVersion = ReadUserVersionValue(connection);
    const int expectedVersion = InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCurrentVersion();

    if (currentVersion == expectedVersion)
    {
        ValidateDatabaseShape(connection);
        if (requireCatalogSingletons)
        {
            ValidateCatalogSingletons(connection);
        }
        return;
    }

    if (currentVersion < 0 || currentVersion > expectedVersion)
    {
        throw std::runtime_error(
            std::format(
                "Unsupported database schema version {} (expected {}). "
                "Delete the cache database so it can be recreated.",
                currentVersion,
                expectedVersion));
    }

    if (currentVersion == 0 && HasUserSchemaObjects(connection))
    {
        throw std::runtime_error(
            "Database schema version 0 is damaged: partially initialized databases must be deleted and recreated.");
    }

    connection.Execute("PRAGMA journal_mode = WAL;");
    connection.Execute("BEGIN IMMEDIATE;");

    try
    {
        connection.Execute(InpxWebReader::DatabaseSchema::CDatabaseSchema::GetCreateSchemaScript());
        if (seedNewCatalog != nullptr)
        {
            std::invoke(*seedNewCatalog, connection);
        }

        ValidateDatabaseShape(connection);
        if (requireCatalogSingletons)
        {
            ValidateCatalogSingletons(connection);
        }
        connection.Execute(std::format("PRAGMA user_version = {};", expectedVersion));
        connection.Execute("COMMIT;");
    }
    catch (const std::exception& primaryError)
    {
        try
        {
            connection.Execute("ROLLBACK;");
        }
        catch (const std::exception& rollbackError)
        {
            InpxWebReader::Logging::ErrorIfInitialized(
                "Database schema initialization rollback failed after '{}': {}",
                primaryError.what(),
                rollbackError.what());
            throw std::runtime_error(
                std::format(
                    "Database schema initialization failed: {}. Rollback also failed: {}.",
                    primaryError.what(),
                    rollbackError.what()));
        }
        catch (...)
        {
            InpxWebReader::Logging::ErrorIfInitialized(
                "Database schema initialization rollback failed after '{}' with a non-standard exception.",
                primaryError.what());
            throw std::runtime_error(
                std::format(
                    "Database schema initialization failed: {}. Rollback also failed with a non-standard exception.",
                    primaryError.what()));
        }

        throw;
    }
    catch (...)
    {
        try
        {
            connection.Execute("ROLLBACK;");
        }
        catch (const std::exception& rollbackError)
        {
            InpxWebReader::Logging::ErrorIfInitialized(
                "Database schema initialization rollback failed after a non-standard exception: {}",
                rollbackError.what());
            throw std::runtime_error(
                std::format(
                    "Database schema initialization failed with a non-standard exception. Rollback also failed: {}.",
                    rollbackError.what()));
        }
        catch (...)
        {
            InpxWebReader::Logging::ErrorIfInitialized(
                "Database schema initialization rollback failed after a non-standard exception with a non-standard rollback exception.");
            throw std::runtime_error(
                "Database schema initialization failed with a non-standard exception. Rollback also failed with a non-standard exception.");
        }

        throw;
    }
}

} // namespace

void CSchemaInitializer::Initialize(const std::filesystem::path& databasePath)
{
    InitializeDatabase(databasePath, nullptr, false);
}

void CSchemaInitializer::InitializeCatalog(
    const std::filesystem::path& databasePath,
    const TCatalogSeed& seedNewCatalog)
{
    if (!seedNewCatalog)
    {
        throw std::invalid_argument("A new catalog seed callback is required.");
    }
    InitializeDatabase(databasePath, &seedNewCatalog, true);
}

int CSchemaInitializer::ReadUserVersion(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    return ReadUserVersionValue(connection);
}

} // namespace InpxWebReader::DatabaseRuntime
