#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Version.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerCommandLine.hpp"
#include "Server/ServerConfig.hpp"

namespace {

volatile std::sig_atomic_t GShutdownRequested = 0;

[[nodiscard]] std::vector<std::string> BuildArgumentsUtf8(const int argc, char** argv)
{
    std::vector<std::string> result;
    result.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index)
    {
        result.emplace_back(argv[index]);
    }
    return result;
}

void HandleShutdownSignal(int)
{
    GShutdownRequested = 1;
}

void InstallShutdownSignalHandlers()
{
    std::signal(SIGINT, HandleShutdownSignal);
    std::signal(SIGTERM, HandleShutdownSignal);
}

void WaitForShutdownSignal()
{
    while (GShutdownRequested == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

[[nodiscard]] InpxWebReader::Logging::ELogLevel ResolveLogLevel(
    const InpxWebReader::Server::EServerLogLevel level)
{
    switch (level)
    {
    case InpxWebReader::Server::EServerLogLevel::Debug:
        return InpxWebReader::Logging::ELogLevel::Debug;
    case InpxWebReader::Server::EServerLogLevel::Info:
        return InpxWebReader::Logging::ELogLevel::Info;
    case InpxWebReader::Server::EServerLogLevel::Warning:
        return InpxWebReader::Logging::ELogLevel::Warning;
    case InpxWebReader::Server::EServerLogLevel::Error:
        return InpxWebReader::Logging::ELogLevel::Error;
    }
    throw std::runtime_error("Unsupported server log level.");
}

[[nodiscard]] std::string_view LogLevelName(
    const InpxWebReader::Server::EServerLogLevel level) noexcept
{
    switch (level)
    {
    case InpxWebReader::Server::EServerLogLevel::Debug:
        return "debug";
    case InpxWebReader::Server::EServerLogLevel::Info:
        return "info";
    case InpxWebReader::Server::EServerLogLevel::Warning:
        return "warning";
    case InpxWebReader::Server::EServerLogLevel::Error:
        return "error";
    }
    return "unknown";
}

void PrintStatus(std::ostream& output, const InpxWebReader::Server::SServerStatus& status)
{
    output << "InpxWebReader " << status.VersionUtf8 << '\n';
    output << "status=" << (status.IsOpen ? "open" : "closed") << '\n';
    output << "canRescanInpxSource=" << (status.Capabilities.CanRescanInpxSource ? "true" : "false") << '\n';
    output << "canDownloadOriginal=" << (status.Capabilities.CanDownloadOriginal ? "true" : "false") << '\n';
    output << "canDownloadAsEpub=" << (status.Capabilities.CanDownloadAsEpub ? "true" : "false") << '\n';
    if (status.InpxSource.has_value())
    {
        output << "inpxSource.inpxPath=" << status.InpxSource->InpxPathUtf8 << '\n';
        output << "inpxSource.archiveRoot=" << status.InpxSource->ArchiveRootUtf8 << '\n';
        output << "inpxSource.available=" << (status.InpxSource->IsSourceAvailable ? "true" : "false") << '\n';
        output << "inpxSource.totalBooks=" << status.InpxSource->TotalBookCount << '\n';
    }
}

void StartConfiguredStartupScan(
    InpxWebReader::Server::CHttpServer& httpServer,
    const InpxWebReader::Server::SServerConfig& config,
    const InpxWebReader::Server::SServerStatus& status)
{
    const auto mode = InpxWebReader::Server::ResolveConfiguredStartupScanMode(config.Startup, status);
    if (!mode.has_value())
    {
        InpxWebReader::Logging::InfoIfInitialized(
            "InpxWebReader startup scan skipped by the configured startup policy or current source state.");
        return;
    }

    const auto startupScan = httpServer.StartScan({
        .Mode = *mode,
        .WarningLimit = 50
    });
    std::cout << "inpxSource.scanJobId=" << startupScan.JobId << '\n';
}

[[nodiscard]] int RunServerMain(const std::vector<std::string>& argumentsUtf8)
{
    try
    {
        const auto launchOptions = InpxWebReader::Server::CServerCommandLineParser::Parse(argumentsUtf8);
        if (launchOptions.Command == InpxWebReader::Server::EServerCommand::ShowHelp)
        {
            std::cout << InpxWebReader::Server::CServerCommandLineParser::BuildHelpText();
            return 0;
        }

        if (launchOptions.Command == InpxWebReader::Server::EServerCommand::ShowVersion)
        {
            std::cout << "InpxWebReader " << InpxWebReader::Core::CVersion::GetValue() << '\n';
            return 0;
        }

        if (!launchOptions.ConfigPath.has_value())
        {
            throw std::runtime_error("Missing required --config <path> option.");
        }

        const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromFile(launchOptions.ConfigPath.value());
        const auto logFilePath = config.RuntimeWorkspaceRoot / "Logs" / "inpx-web-reader.log";
        InpxWebReader::Logging::CLogging::InitializeHostLogger(
            logFilePath,
            {
                .Level = ResolveLogLevel(config.Logging.Level),
                .MaxFileSizeBytes = config.Logging.MaxFileSizeMiB * 1024U * 1024U,
                .MaxRotatedFiles = config.Logging.MaxRotatedFiles
            });
        InstallShutdownSignalHandlers();

        try
        {
            InpxWebReader::Logging::Info(
                "InpxWebReader startup: version={} host={} port={} logFile='{}' "
                "logLevel={} logMaxFileSizeMiB={} logMaxRotatedFiles={}",
                InpxWebReader::Core::CVersion::GetValue(),
                config.Server.HostUtf8,
                config.Server.Port,
                InpxWebReader::Unicode::PathToUtf8(logFilePath),
                LogLevelName(config.Logging.Level),
                config.Logging.MaxFileSizeMiB,
                config.Logging.MaxRotatedFiles);

            InpxWebReader::Server::CServerApplicationHost host(config);
            InpxWebReader::Server::CHttpServer httpServer(
                host,
                InpxWebReader::Server::BuildHttpServerOptions(config));
            httpServer.Start();

            std::cout << "InpxWebReader listening on http://"
                      << config.Server.HostUtf8 << ':'
                      << httpServer.GetBoundPort() << '\n';
            host.Open();
            const auto status = host.GetStatus();
            PrintStatus(std::cout, status);
            StartConfiguredStartupScan(httpServer, config, status);
            WaitForShutdownSignal();

            httpServer.Stop();
            host.Close();

            InpxWebReader::Logging::Info("InpxWebReader shutdown complete.");
            InpxWebReader::Logging::CLogging::Shutdown();
            return 0;
        }
        catch (const std::exception& ex)
        {
            InpxWebReader::Logging::Error("InpxWebReader failed: {}", ex.what());
            InpxWebReader::Logging::CLogging::Shutdown();
            return 1;
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    return RunServerMain(BuildArgumentsUtf8(argc, argv));
}
