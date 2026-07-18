#include "Foundation/Logging.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Logging {
namespace {

std::mutex LoggerMutex;
std::shared_ptr<spdlog::logger> LoggerInstance;
constexpr std::string_view LoggerName = "Server";
constexpr std::string_view LogPattern = "[%Y-%m-%dT%H:%M:%S.%eZ] [%^%l%$] [%n] %v";

[[nodiscard]] spdlog::filename_t ToSpdlogFilename(const std::filesystem::path& path)
{
    return InpxWebReader::Unicode::PathToUtf8(path);
}

[[nodiscard]] std::shared_ptr<spdlog::logger> CreateLogger(
    std::string_view name,
    const std::vector<spdlog::sink_ptr>& sinks,
    const ELogLevel level)
{
    auto logger = std::make_shared<spdlog::logger>(
        std::string{name},
        sinks.begin(),
        sinks.end());
    switch (level)
    {
    case ELogLevel::Debug:
        logger->set_level(spdlog::level::debug);
        break;
    case ELogLevel::Info:
        logger->set_level(spdlog::level::info);
        break;
    case ELogLevel::Warning:
        logger->set_level(spdlog::level::warn);
        break;
    case ELogLevel::Error:
        logger->set_level(spdlog::level::err);
        break;
    }
    logger->flush_on(spdlog::level::warn);
    return logger;
}

void SetUtcPattern(const spdlog::sink_ptr& sink)
{
    sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        std::string{LogPattern},
        spdlog::pattern_time_type::utc));
}

[[nodiscard]] std::shared_ptr<spdlog::logger> GetRequiredLogger(
    const std::shared_ptr<spdlog::logger>& logger)
{
    if (!logger)
    {
        throw std::runtime_error("InpxWebReader logger is not initialized.");
    }

    return logger;
}

} // namespace

void CLogging::InitializeHostLogger(
    const std::filesystem::path& logFilePath,
    const SLoggingOptions& options)
{
    std::scoped_lock lock(LoggerMutex);

    if (options.MaxFileSizeBytes == 0 || options.MaxRotatedFiles == 0)
    {
        throw std::invalid_argument("Logging rotation limits must be at least 1.");
    }

    std::filesystem::create_directories(logFilePath.parent_path());

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        ToSpdlogFilename(logFilePath),
        options.MaxFileSizeBytes,
        options.MaxRotatedFiles);
    auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

    SetUtcPattern(fileSink);
    SetUtcPattern(stderrSink);

    std::vector<spdlog::sink_ptr> sinks{
        std::move(fileSink),
        std::move(stderrSink)
    };

    LoggerInstance = CreateLogger(LoggerName, sinks, options.Level);
    spdlog::set_default_logger(LoggerInstance);
    spdlog::flush_every(std::chrono::seconds{1});
}

void CLogging::Shutdown() noexcept
{
    std::scoped_lock lock(LoggerMutex);
    LoggerInstance.reset();
    spdlog::shutdown();
}

std::unique_lock<std::mutex> CLogging::LockLoggerState()
{
    return std::unique_lock{LoggerMutex};
}

std::shared_ptr<spdlog::logger> CLogging::LoggerUnlocked() noexcept
{
    return LoggerInstance;
}

std::shared_ptr<spdlog::logger> CLogging::RequiredLoggerUnlocked()
{
    return GetRequiredLogger(LoggerInstance);
}

} // namespace InpxWebReader::Logging
