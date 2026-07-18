#pragma once

#include <filesystem>
#include <functional>

namespace InpxWebReader::Sqlite {
class CSqliteConnection;
}

namespace InpxWebReader::DatabaseRuntime {

class CSchemaInitializer
{
public:
    using TCatalogSeed = std::function<void(const InpxWebReader::Sqlite::CSqliteConnection&)>;

    static void Initialize(const std::filesystem::path& databasePath);
    static void InitializeCatalog(
        const std::filesystem::path& databasePath,
        const TCatalogSeed& seedNewCatalog);
    [[nodiscard]] static int ReadUserVersion(const std::filesystem::path& databasePath);
};

} // namespace InpxWebReader::DatabaseRuntime
