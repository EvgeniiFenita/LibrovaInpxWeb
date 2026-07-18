#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <system_error>

#include "App/FileReplacement.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Logging.hpp"
#include "TestWorkspace.hpp"

namespace {

std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("ReplaceDestinationWithPreparedFile reports recoverable backup path when restore fails", "[application][files]")
{
    const auto destinationPath = std::filesystem::path("/virtual/destination.epub");
    const auto preparedPath = std::filesystem::path("/virtual/prepared.epub");
    auto backupPath = std::filesystem::path{};

    bool destinationExists = true;
    bool backupExists = false;

    InpxWebReader::Application::SFileReplacementHooks hooks;
    hooks.Exists = [&](const std::filesystem::path& path)
    {
        if (path == destinationPath)
        {
            return destinationExists;
        }

        if (path == backupPath)
        {
            return backupExists;
        }

        return path == preparedPath;
    };
    hooks.Rename = [&](const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (source == destinationPath && destination.extension() == ".inpx-web-reader-backup")
        {
            backupPath = destination;
            destinationExists = false;
            backupExists = true;
            return;
        }

        if (source == preparedPath && destination == destinationPath)
        {
            throw std::runtime_error("prepared rename failed");
        }

        FAIL("Unexpected rename call.");
    };
    hooks.RenameNoThrow = [&](const std::filesystem::path& source, const std::filesystem::path& destination, std::error_code& errorCode)
    {
        REQUIRE(source == backupPath);
        REQUIRE(destination == destinationPath);
        errorCode = std::make_error_code(std::errc::permission_denied);
    };

    try
    {
        InpxWebReader::Application::ReplaceDestinationWithPreparedFile(
            preparedPath,
            destinationPath,
            "PrepareDownload",
            hooks);
        FAIL("Expected restore failure exception.");
    }
    catch (const InpxWebReader::Application::CFileReplacementRestoreException& error)
    {
        REQUIRE(error.GetBackupPath() == backupPath);
        REQUIRE(error.GetDestinationPath() == destinationPath);
        REQUIRE_THAT(error.what(), Catch::Matchers::ContainsSubstring("prepared rename failed"));
        REQUIRE_THAT(
            error.what(),
            Catch::Matchers::ContainsSubstring("permission denied", Catch::CaseSensitive::No));
        REQUIRE_THAT(
            error.what(),
            Catch::Matchers::ContainsSubstring(InpxWebReader::Unicode::PathToUtf8(backupPath)));
    }
}

TEST_CASE("ReplaceDestinationWithPreparedFile retains the backup when published validation rollback fails", "[application][files]")
{
    const auto destinationPath = std::filesystem::path("/virtual/destination.epub");
    const auto preparedPath = std::filesystem::path("/virtual/prepared.epub");
    auto backupPath = std::filesystem::path{};

    bool destinationExists = true;
    bool preparedExists = true;
    bool backupExists = false;

    InpxWebReader::Application::SFileReplacementHooks hooks;
    hooks.Exists = [&](const std::filesystem::path& path)
    {
        if (path == destinationPath)
        {
            return destinationExists;
        }
        if (path == preparedPath)
        {
            return preparedExists;
        }
        if (path == backupPath)
        {
            return backupExists;
        }
        return false;
    };
    hooks.Rename = [&](const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (source == destinationPath && destination.extension() == ".inpx-web-reader-backup")
        {
            backupPath = destination;
            destinationExists = false;
            backupExists = true;
            return;
        }
        if (source == preparedPath && destination == destinationPath)
        {
            preparedExists = false;
            destinationExists = true;
            return;
        }
        FAIL("Unexpected rename call.");
    };
    hooks.RemoveNoThrow = [&](const std::filesystem::path& path, std::error_code& errorCode)
    {
        REQUIRE(path == destinationPath);
        destinationExists = false;
        errorCode.clear();
    };
    hooks.RenameNoThrow = [&](const std::filesystem::path& source, const std::filesystem::path& destination, std::error_code& errorCode)
    {
        REQUIRE(source == backupPath);
        REQUIRE(destination == destinationPath);
        errorCode = std::make_error_code(std::errc::permission_denied);
    };

    try
    {
        InpxWebReader::Application::ReplaceDestinationWithPreparedFile(
            preparedPath,
            destinationPath,
            "PrepareDownload",
            hooks,
            [] { throw std::runtime_error("published validation failed"); });
        FAIL("Expected restore failure exception.");
    }
    catch (const InpxWebReader::Application::CFileReplacementRestoreException& error)
    {
        REQUIRE(error.GetBackupPath() == backupPath);
        REQUIRE(error.GetDestinationPath() == destinationPath);
        REQUIRE(backupExists);
        REQUIRE_FALSE(destinationExists);
        REQUIRE_FALSE(preparedExists);
        REQUIRE_THAT(error.what(), Catch::Matchers::ContainsSubstring("published validation failed"));
        REQUIRE_THAT(
            error.what(),
            Catch::Matchers::ContainsSubstring("permission denied", Catch::CaseSensitive::No));
    }
}

TEST_CASE("ReplaceDestinationWithPreparedFile logs backup cleanup failure after a successful replace", "[application][files]")
{
    CTestWorkspace workspace("inpx-web-reader-file-replacement-backup-cleanup-log");
    const auto logPath = workspace.GetPath() / "file-replacement.log";
    const auto destinationPath = std::filesystem::path("/virtual/destination.epub");
    const auto preparedPath = std::filesystem::path("/virtual/prepared.epub");
    auto backupPath = std::filesystem::path{};

    bool destinationExists = true;
    bool backupExists = false;

    InpxWebReader::Application::SFileReplacementHooks hooks;
    hooks.Exists = [&](const std::filesystem::path& path)
    {
        if (path == destinationPath)
        {
            return destinationExists;
        }

        if (path == backupPath)
        {
            return backupExists;
        }

        return path == preparedPath;
    };
    hooks.Rename = [&](const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (source == destinationPath && destination.extension() == ".inpx-web-reader-backup")
        {
            backupPath = destination;
            destinationExists = false;
            backupExists = true;
            return;
        }

        if (source == preparedPath && destination == destinationPath)
        {
            destinationExists = true;
            return;
        }

        FAIL("Unexpected rename call.");
    };
    hooks.RemoveNoThrow = [&](const std::filesystem::path& path, std::error_code& errorCode)
    {
        REQUIRE(path == backupPath);
        errorCode = std::make_error_code(std::errc::permission_denied);
    };

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::Application::ReplaceDestinationWithPreparedFile(
            preparedPath,
            destinationPath,
            "PrepareDownload",
            hooks);
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    REQUIRE_THAT(
        ReadAllText(logPath),
        Catch::Matchers::ContainsSubstring("failed to remove replacement backup after a successful replace"));
}
