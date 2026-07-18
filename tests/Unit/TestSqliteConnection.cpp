#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <thread>

#include "Database/DatabaseSchema.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "TestWorkspace.hpp"

TEST_CASE("Sqlite connection can initialize the INPX schema in a temporary database", "[sqlite]")
{
    const auto databasePath = MakeUniqueTestPath("inpx-web-reader-sqlite-smoke.db");
    std::filesystem::remove(databasePath);

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);

        for (const std::string_view statement : InpxWebReader::DatabaseSchema::CDatabaseSchema::GetInitializationStatements())
        {
            connection.Execute(statement);
        }

        connection.Execute("INSERT INTO inpx_books (id, title, normalized_title, language, added_at_utc) "
                           "VALUES (1, 'Roadside Picnic', 'roadside picnic', 'ru', '2026-03-30T12:00:00Z');");

        InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), "SELECT COUNT(*) FROM inpx_books;");

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumnInt(0) == 1);
        REQUIRE_FALSE(statement.Step());
    }

    std::filesystem::remove(databasePath);
}
TEST_CASE("Sqlite connection enables foreign key enforcement for each new connection", "[sqlite]")
{
    const auto databasePath = MakeUniqueTestPath("inpx-web-reader-sqlite-foreign-keys.db");
    std::filesystem::remove(databasePath);

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), "PRAGMA foreign_keys;");

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumnInt(0) == 1);
        REQUIRE_FALSE(statement.Step());
    }

    std::filesystem::remove(databasePath);
}

TEST_CASE("Sqlite connection configures busy timeout for each new connection", "[sqlite]")
{
    const auto databasePath = MakeUniqueTestPath("inpx-web-reader-sqlite-busy-timeout.db");
    std::filesystem::remove(databasePath);

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), "PRAGMA busy_timeout;");

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumnInt(0) == 5000);
        REQUIRE_FALSE(statement.Step());
    }

    std::filesystem::remove(databasePath);
}

TEST_CASE("Sqlite statement step surfaces sqlite error text", "[sqlite]")
{
    const auto databasePath = MakeUniqueTestPath("inpx-web-reader-sqlite-step-error-text.db");
    std::filesystem::remove(databasePath);

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        connection.Execute("CREATE TABLE step_error_test(id INTEGER PRIMARY KEY, value TEXT NOT NULL);");
        connection.Execute("INSERT INTO step_error_test(id, value) VALUES (1, 'first');");

        InpxWebReader::Sqlite::CSqliteStatement statement(
            connection.GetNativeHandle(),
            "INSERT INTO step_error_test(id, value) VALUES (1, 'duplicate');");

        try
        {
            static_cast<void>(statement.Step());
            FAIL("Expected sqlite step failure to throw.");
        }
        catch (const std::runtime_error& ex)
        {
            const std::string message = ex.what();
            REQUIRE(message.find("Failed to step sqlite statement:") != std::string::npos);
            REQUIRE(message.find("UNIQUE constraint failed") != std::string::npos);
        }
    }

    std::filesystem::remove(databasePath);
}

TEST_CASE("Sqlite connection opens a database under a Unicode path", "[sqlite]")
{
    const auto sandboxPath = MakeUniqueTestPath("inpx-web-reader-тест-sqlite");
    const std::filesystem::path databasePath = sandboxPath / std::filesystem::path{u8"данные.db"};

    std::filesystem::remove_all(sandboxPath);
    std::filesystem::create_directories(sandboxPath);

    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);

        for (const std::string_view statement : InpxWebReader::DatabaseSchema::CDatabaseSchema::GetInitializationStatements())
        {
            connection.Execute(statement);
        }

        connection.Execute("INSERT INTO inpx_books (id, title, normalized_title, language, added_at_utc) "
                           "VALUES (1, 'Metro 2033', 'metro 2033', 'ru', '2026-03-30T12:00:00Z');");

        InpxWebReader::Sqlite::CSqliteStatement statement(connection.GetNativeHandle(), "SELECT title FROM inpx_books WHERE id = 1;");

        REQUIRE(statement.Step());
        REQUIRE(statement.GetColumnText(0) == "Metro 2033");
        REQUIRE_FALSE(statement.Step());
    }

    std::error_code errorCode;
    std::filesystem::remove_all(sandboxPath, errorCode);
}

TEST_CASE("Sqlite connection waits for an overlapping write transaction instead of failing immediately", "[sqlite]")
{
    const auto databasePath = MakeUniqueTestPath("inpx-web-reader-sqlite-write-contention.db");
    std::filesystem::remove(databasePath);

    {
        InpxWebReader::Sqlite::CSqliteConnection setupConnection(databasePath);
        setupConnection.Execute("CREATE TABLE contention_test(id INTEGER PRIMARY KEY, value TEXT NOT NULL);");
    }

    {
        InpxWebReader::Sqlite::CSqliteConnection firstConnection(databasePath);
        firstConnection.Execute("BEGIN IMMEDIATE;");
        firstConnection.Execute("INSERT INTO contention_test(id, value) VALUES (1, 'first');");

        std::atomic<bool> secondInsertSucceeded = false;
        std::atomic<bool> secondInsertStarted = false;
        std::exception_ptr workerFailure;

        std::jthread worker([&] {
            try
            {
                secondInsertStarted.store(true);

                InpxWebReader::Sqlite::CSqliteConnection secondConnection(databasePath);
                secondConnection.Execute("BEGIN IMMEDIATE;");
                secondConnection.Execute("INSERT INTO contention_test(id, value) VALUES (2, 'second');");
                secondConnection.Execute("COMMIT;");
                secondInsertSucceeded.store(true);
            }
            catch (...)
            {
                workerFailure = std::current_exception();
            }
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!secondInsertStarted.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(secondInsertStarted.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        REQUIRE_FALSE(secondInsertSucceeded.load());

        firstConnection.Execute("COMMIT;");
        worker.join();

        if (workerFailure != nullptr)
        {
            std::rethrow_exception(workerFailure);
        }

        REQUIRE(secondInsertSucceeded.load());

        {
            InpxWebReader::Sqlite::CSqliteConnection verifyConnection(databasePath);
            InpxWebReader::Sqlite::CSqliteStatement statement(
                verifyConnection.GetNativeHandle(),
                "SELECT COUNT(*) FROM contention_test;");

            REQUIRE(statement.Step());
            REQUIRE(statement.GetColumnInt(0) == 2);
            REQUIRE_FALSE(statement.Step());
        }
    }

    std::filesystem::remove(databasePath);
}
