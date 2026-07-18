#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "Database/SqliteConnection.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Foundation/Logging.hpp"
#include "TestWorkspace.hpp"

namespace {

[[nodiscard]] std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Sqlite transaction logs rollback cleanup failures instead of swallowing them silently", "[database-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-sqlite-transaction-rollback-log");
    const auto databasePath = sandbox.GetPath() / "inpx-web-reader.db";
    const auto logPath = sandbox.GetPath() / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        {
            InpxWebReader::Sqlite::CSqliteTransaction transaction(connection);
            connection.Execute("ROLLBACK;");
        }
    }
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(std::filesystem::exists(logPath));
    REQUIRE(ReadAllText(logPath).find("SQLite transaction rollback failed:") != std::string::npos);
}
