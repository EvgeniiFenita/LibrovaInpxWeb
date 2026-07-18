#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

namespace InpxWebReader::Logging {

enum class ELogLevel
{
    Debug,
    Info,
    Warning,
    Error
};

struct SLoggingOptions
{
    ELogLevel Level = ELogLevel::Info;
    std::size_t MaxFileSizeBytes = static_cast<std::size_t>(20) * 1024U * 1024U;
    std::size_t MaxRotatedFiles = 4;
};

class CLogging final
{
public:
    static void InitializeHostLogger(
        const std::filesystem::path& logFilePath,
        const SLoggingOptions& options = {});
    static void Shutdown() noexcept;

    template <typename... TArgs>
    static void Log(spdlog::level::level_enum level, spdlog::format_string_t<TArgs...> format, TArgs&&... args)
    {
        auto lock = LockLoggerState();
        RequiredLoggerUnlocked()->log(level, format, std::forward<TArgs>(args)...);
    }

    template <typename... TArgs>
    static void LogIfInitialized(
        spdlog::level::level_enum level,
        spdlog::format_string_t<TArgs...> format,
        TArgs&&... args)
    {
        auto lock = LockLoggerState();
        if (auto logger = LoggerUnlocked())
        {
            logger->log(level, format, std::forward<TArgs>(args)...);
        }
    }

private:
    [[nodiscard]] static std::unique_lock<std::mutex> LockLoggerState();
    [[nodiscard]] static std::shared_ptr<spdlog::logger> LoggerUnlocked() noexcept;
    [[nodiscard]] static std::shared_ptr<spdlog::logger> RequiredLoggerUnlocked();
};

template <typename... TArgs>
void Info(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::Log(spdlog::level::info, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void Debug(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::Log(spdlog::level::debug, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void Warn(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::Log(spdlog::level::warn, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void Error(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::Log(spdlog::level::err, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void Critical(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::Log(spdlog::level::critical, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void InfoIfInitialized(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::LogIfInitialized(spdlog::level::info, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void DebugIfInitialized(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::LogIfInitialized(spdlog::level::debug, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void WarnIfInitialized(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::LogIfInitialized(spdlog::level::warn, format, std::forward<TArgs>(args)...);
}

template <typename... TArgs>
void ErrorIfInitialized(spdlog::format_string_t<TArgs...> format, TArgs&&... args)
{
    CLogging::LogIfInitialized(spdlog::level::err, format, std::forward<TArgs>(args)...);
}

} // namespace InpxWebReader::Logging
