#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>

#include "Converter/ConverterCommandBuilder.hpp"

namespace InpxWebReader::Domain {
class IProgressSink;
}

namespace InpxWebReader::ConverterRuntime {

struct SExternalConverterExecutableIdentity
{
    std::filesystem::path CanonicalPath;
    std::uint64_t Device = 0;
    std::uint64_t Inode = 0;
    std::uint64_t SizeBytes = 0;
    std::int64_t ModifiedSeconds = 0;
    std::int64_t ModifiedNanoseconds = 0;
    std::int64_t ChangedSeconds = 0;
    std::int64_t ChangedNanoseconds = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return !CanonicalPath.empty() && Inode != 0;
    }

    bool operator==(const SExternalConverterExecutableIdentity&) const = default;
};

[[nodiscard]] SExternalConverterExecutableIdentity ReadExternalConverterExecutableIdentity(
    const std::filesystem::path& executablePath);

struct SExternalConverterProcessHooks
{
    std::function<void(std::uint32_t processId)> AfterProcessCreatedBeforeJob;
};

struct SExternalConverterProcessRequest
{
    InpxWebReader::ConverterCommand::SResolvedConverterCommand Command;
    std::filesystem::path WorkingDirectory;
    std::chrono::milliseconds PollInterval = std::chrono::milliseconds{100};
    std::chrono::milliseconds Timeout = std::chrono::minutes{5};
    std::optional<SExternalConverterExecutableIdentity> ExpectedExecutableIdentity = std::nullopt;
    SExternalConverterProcessHooks ProcessHooks;
};

struct SExternalConverterProcessResult
{
    std::uint32_t ExitCode = 0;
    bool WasCancelled = false;
    bool WasTimedOut = false;
};

class IExternalConverterProcessRunner
{
public:
    virtual ~IExternalConverterProcessRunner() = default;

    [[nodiscard]] virtual SExternalConverterProcessResult Run(
        const SExternalConverterProcessRequest& request,
        InpxWebReader::Domain::IProgressSink& progressSink,
        std::stop_token stopToken) const = 0;
};

} // namespace InpxWebReader::ConverterRuntime
