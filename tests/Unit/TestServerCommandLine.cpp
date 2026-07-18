#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "Foundation/UnicodeConversion.hpp"
#include "Server/ServerCommandLine.hpp"
#include "TestWorkspace.hpp"

namespace {

struct SProcessResult
{
    int ExitCode = -1;
    std::string StandardOutput;
    std::string StandardError;
};

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

SProcessResult RunServerProcess(CTestWorkspace& workspace, const std::vector<std::string>& arguments)
{
    const auto stdoutPath = workspace.GetPath() / "stdout.txt";
    const auto stderrPath = workspace.GetPath() / "stderr.txt";
    const pid_t child = fork();
    if (child < 0)
    {
        throw std::runtime_error("Failed to fork server CLI test process.");
    }
    if (child == 0)
    {
        const int stdoutFile = open(stdoutPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        const int stderrFile = open(stderrPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (stdoutFile < 0 || stderrFile < 0
            || dup2(stdoutFile, STDOUT_FILENO) < 0
            || dup2(stderrFile, STDERR_FILENO) < 0)
        {
            _exit(126);
        }
        close(stdoutFile);
        close(stderrFile);

        std::vector<std::string> ownedArguments{INPX_WEB_READER_EXECUTABLE_PATH};
        ownedArguments.insert(ownedArguments.end(), arguments.begin(), arguments.end());
        std::vector<char*> rawArguments;
        rawArguments.reserve(ownedArguments.size() + 1);
        for (auto& argument : ownedArguments)
        {
            rawArguments.push_back(argument.data());
        }
        rawArguments.push_back(nullptr);
        execv(rawArguments.front(), rawArguments.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child)
    {
        throw std::runtime_error("Failed to wait for server CLI test process.");
    }
    return {
        .ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
        .StandardOutput = ReadAll(stdoutPath),
        .StandardError = ReadAll(stderrPath)
    };
}

} // namespace

TEST_CASE("Server command line handles help before config is required", "[server][cli]")
{
    const auto parsed = InpxWebReader::Server::CServerCommandLineParser::Parse({"--help"});

    REQUIRE(parsed.Command == InpxWebReader::Server::EServerCommand::ShowHelp);
    REQUIRE_FALSE(parsed.ConfigPath.has_value());
}

TEST_CASE("Server command line handles short aliases", "[server][cli]")
{
    REQUIRE(InpxWebReader::Server::CServerCommandLineParser::Parse({"-h"}).Command
        == InpxWebReader::Server::EServerCommand::ShowHelp);
    REQUIRE(InpxWebReader::Server::CServerCommandLineParser::Parse({"-v"}).Command
        == InpxWebReader::Server::EServerCommand::ShowVersion);
}

TEST_CASE("Server command line handles version before config is required", "[server][cli]")
{
    const auto parsed = InpxWebReader::Server::CServerCommandLineParser::Parse({"--version"});

    REQUIRE(parsed.Command == InpxWebReader::Server::EServerCommand::ShowVersion);
    REQUIRE_FALSE(parsed.ConfigPath.has_value());
}

TEST_CASE("Server command line parses UTF-8 config path through Foundation helpers", "[server][cli]")
{
    const auto parsed = InpxWebReader::Server::CServerCommandLineParser::Parse({
        "--config",
        "C:/data/конфиг/server.json"
    });

    REQUIRE(parsed.Command == InpxWebReader::Server::EServerCommand::Run);
    REQUIRE(parsed.ConfigPath.has_value());
    REQUIRE(
        InpxWebReader::Unicode::PathToUtf8(*parsed.ConfigPath)
        == std::string{"C:/data/конфиг/server.json"});
}

TEST_CASE("Server command line rejects missing config for run mode", "[server][cli]")
{
    REQUIRE_THROWS_AS(InpxWebReader::Server::CServerCommandLineParser::Parse({}), std::runtime_error);
    REQUIRE_THROWS_AS(InpxWebReader::Server::CServerCommandLineParser::Parse({"--config"}), std::runtime_error);
    REQUIRE_THROWS_AS(InpxWebReader::Server::CServerCommandLineParser::Parse({"--unknown"}), std::runtime_error);
    REQUIRE_THROWS_AS(
        InpxWebReader::Server::CServerCommandLineParser::Parse({"--config", "one.json", "--config", "two.json"}),
        std::runtime_error);
}

TEST_CASE("Server executable reports help and version through both aliases", "[server][cli][process]")
{
    CTestWorkspace workspace("inpx-web-reader-cli-output");

    for (const std::string& option : {std::string{"--help"}, std::string{"-h"}})
    {
        const auto result = RunServerProcess(workspace, {option});
        REQUIRE(result.ExitCode == 0);
        REQUIRE(result.StandardOutput.find("Usage: inpx-web-reader --config <path>") != std::string::npos);
        REQUIRE(result.StandardError.empty());
    }

    for (const std::string& option : {std::string{"--version"}, std::string{"-v"}})
    {
        const auto result = RunServerProcess(workspace, {option});
        REQUIRE(result.ExitCode == 0);
        REQUIRE(result.StandardOutput.find("InpxWebReader ") == 0);
        REQUIRE(result.StandardError.empty());
    }
}

TEST_CASE("Server executable reports command and config failures on stderr", "[server][cli][process]")
{
    CTestWorkspace workspace("inpx-web-reader-cli-errors");

    const auto missingConfig = RunServerProcess(workspace, {});
    REQUIRE(missingConfig.ExitCode == 1);
    REQUIRE(missingConfig.StandardOutput.empty());
    REQUIRE(missingConfig.StandardError.find("Missing required --config") != std::string::npos);

    const auto unknownOption = RunServerProcess(workspace, {"--unknown"});
    REQUIRE(unknownOption.ExitCode == 1);
    REQUIRE(unknownOption.StandardError.find("Unknown server command line option") != std::string::npos);

    const auto configPath = workspace.GetPath() / "invalid.json";
    {
        std::ofstream config(configPath, std::ios::binary);
        config << "{invalid";
    }
    const auto invalidConfig = RunServerProcess(
        workspace,
        {"--config", InpxWebReader::Unicode::PathToUtf8(configPath)});
    REQUIRE(invalidConfig.ExitCode == 1);
    REQUIRE(invalidConfig.StandardOutput.empty());
    REQUIRE_FALSE(invalidConfig.StandardError.empty());
}
