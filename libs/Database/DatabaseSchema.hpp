#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace InpxWebReader::DatabaseSchema {

struct SRequiredSchemaObject
{
    std::string_view Type;
    std::string_view Name;
};

struct SRequiredTableColumn
{
    std::string_view Name;
    std::string_view DeclaredType;
    bool NotNull = false;
    int PrimaryKeyOrdinal = 0;
    std::optional<std::string_view> DefaultValue = std::nullopt;
};

struct SRequiredForeignKey
{
    std::string_view FromColumn;
    std::string_view ReferencedTable;
    std::string_view ReferencedColumn;
    std::string_view OnUpdateAction = "NO ACTION";
    std::string_view OnDeleteAction = "NO ACTION";
};

struct SRequiredTableShape
{
    std::string_view Name;
    std::vector<SRequiredTableColumn> Columns;
    std::vector<SRequiredForeignKey> ForeignKeys;
    std::vector<std::string_view> RequiredSqlTokens;
};

struct SRequiredIndexShape
{
    std::string_view Name;
    std::vector<std::string_view> Columns;
};

struct SFtsTableShape
{
    std::string_view Name;
    std::vector<std::string_view> Columns;
    std::vector<std::string_view> RequiredSqlTokens;
};

class CDatabaseSchema
{
public:
    [[nodiscard]] static int GetCurrentVersion() noexcept;
    [[nodiscard]] static const std::vector<std::string_view>& GetInitializationStatements();
    [[nodiscard]] static std::string_view GetCreateSchemaScript() noexcept;
    [[nodiscard]] static const std::vector<SRequiredSchemaObject>& GetRequiredObjects();
    [[nodiscard]] static const std::vector<SRequiredTableShape>& GetRequiredTableShapes();
    [[nodiscard]] static const std::vector<SRequiredIndexShape>& GetRequiredIndexShapes();
    [[nodiscard]] static const SFtsTableShape& GetRequiredSearchIndexShape();
};

} // namespace InpxWebReader::DatabaseSchema
