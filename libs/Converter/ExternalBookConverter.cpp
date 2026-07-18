#include "Converter/ExternalBookConverter.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Converter/BuiltInConverterCliValidation.hpp"
#include "Converter/ExternalBookConverterFileMove.hpp"
#include "Converter/LinuxExternalConverterProcessRunner.hpp"
#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ConverterRuntime {
namespace {

class CScopedDirectoryCleanup final
{
public:
    explicit CScopedDirectoryCleanup(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~CScopedDirectoryCleanup() noexcept
    {
        InpxWebReader::Foundation::RemovePathBestEffortNoThrow(m_path);
    }

    CScopedDirectoryCleanup(const CScopedDirectoryCleanup&) = delete;
    CScopedDirectoryCleanup& operator=(const CScopedDirectoryCleanup&) = delete;

private:
    std::filesystem::path m_path;
};

const IExternalConverterProcessRunner& GetDefaultProcessRunner()
{
    static const CLinuxExternalConverterProcessRunner runner;
    return runner;
}

std::set<std::filesystem::path> SnapshotDirectoryEntries(const std::filesystem::path& directoryPath)
{
    std::set<std::filesystem::path> snapshot;

    if (!std::filesystem::exists(directoryPath))
    {
        return snapshot;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directoryPath))
    {
        snapshot.insert(entry.path().lexically_normal());
    }

    return snapshot;
}

struct SProducedOutputResolution
{
    std::optional<std::filesystem::path> Path;
    bool UnsafeCandidate = false;
};

SProducedOutputResolution ResolveSingleProducedFile(
    const std::filesystem::path& directoryPath,
    const std::set<std::filesystem::path>& beforeSnapshot,
    const std::filesystem::path& expectedOutputPath)
{
    std::vector<std::filesystem::path> eligibleOutputs;
    const std::filesystem::path normalizedExpectedPath = expectedOutputPath.lexically_normal();

    if (std::filesystem::exists(directoryPath))
    {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directoryPath))
        {
            const std::filesystem::path currentPath = entry.path().lexically_normal();
            if (beforeSnapshot.contains(currentPath))
            {
                continue;
            }
            const bool exactPath = currentPath == normalizedExpectedPath;
            if (!exactPath && currentPath.extension() != expectedOutputPath.extension())
            {
                continue;
            }
            if (!IsGeneratedOutputRegularFile(currentPath))
            {
                return {.UnsafeCandidate = true};
            }
            eligibleOutputs.push_back(currentPath);
        }
    }

    if (eligibleOutputs.size() == 1)
    {
        return {.Path = std::move(eligibleOutputs.front())};
    }
    return {};
}

void CleanupProducedEntries(
    const std::filesystem::path& directoryPath,
    const std::set<std::filesystem::path>& beforeSnapshot,
    const std::optional<std::filesystem::path>& preservedPath = std::nullopt) noexcept
{
    std::error_code errorCode;
    if (!std::filesystem::exists(directoryPath, errorCode) || errorCode)
    {
        return;
    }

    std::vector<std::filesystem::path> pathsToRemove;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directoryPath, errorCode))
    {
        if (errorCode)
        {
            return;
        }

        const std::filesystem::path currentPath = entry.path().lexically_normal();
        if (beforeSnapshot.contains(currentPath)
            || (preservedPath.has_value() && currentPath == preservedPath->lexically_normal()))
        {
            continue;
        }
        pathsToRemove.push_back(currentPath);
    }
    for (const auto& path : pathsToRemove)
    {
        InpxWebReader::Foundation::RemovePathBestEffortNoThrow(path);
    }
}

InpxWebReader::Domain::SConversionResult BuildCancelledResult()
{
    return {
        .Status = InpxWebReader::Domain::EConversionStatus::Cancelled,
        .Warnings = {"Conversion cancelled."}
    };
}

InpxWebReader::Domain::SConversionResult BuildFailedResult(const std::string& warning)
{
    return {
        .Status = InpxWebReader::Domain::EConversionStatus::Failed,
        .Warnings = {warning}
    };
}

InpxWebReader::Domain::SConversionResult BuildTimedOutResult()
{
    return {
        .Status = InpxWebReader::Domain::EConversionStatus::TimedOut,
        .Warnings = {"Conversion timed out."}
    };
}

} // namespace

CExternalBookConverter::CExternalBookConverter(SExternalConverterSettings settings)
    : m_settings(std::move(settings))
    , m_processRunner(m_settings.ProcessRunner != nullptr ? m_settings.ProcessRunner : &GetDefaultProcessRunner())
{
    if (!m_settings.IsValid())
    {
        throw std::invalid_argument("External converter settings must contain a valid command profile and poll interval.");
    }
}

bool CExternalBookConverter::CanConvert(
    const InpxWebReader::Domain::EBookFormat sourceFormat,
    const InpxWebReader::Domain::EBookFormat destinationFormat) const
{
    return sourceFormat == InpxWebReader::Domain::EBookFormat::Fb2
        && destinationFormat == InpxWebReader::Domain::EBookFormat::Epub;
}

InpxWebReader::Domain::SConversionResult CExternalBookConverter::Convert(
    const InpxWebReader::Domain::SConversionRequest& request,
    InpxWebReader::Domain::IProgressSink& progressSink,
    const std::stop_token stopToken) const
{
    if (!CanConvert(request.SourceFormat, request.DestinationFormat))
    {
        return BuildFailedResult("Requested conversion direction is not supported.");
    }

    const std::filesystem::path processWorkingDirectory = m_settings.WorkingDirectory.empty()
        ? request.DestinationPath.parent_path()
        : InpxWebReader::Foundation::CreateUniqueDirectory(
              m_settings.WorkingDirectory,
              "conversion-");
    CScopedDirectoryCleanup workingDirectoryCleanup(
        m_settings.WorkingDirectory.empty() ? std::filesystem::path{} : processWorkingDirectory);
    const bool stageGeneratedOutputInWorkingDirectory =
        m_settings.CommandProfile.OutputMode == InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory
        && !m_settings.WorkingDirectory.empty();
    const std::filesystem::path commandOutputDirectory =
        stageGeneratedOutputInWorkingDirectory
            ? processWorkingDirectory / "generated-output"
            : request.DestinationPath.parent_path();

    std::optional<SExternalConverterExecutableIdentity> executableIdentity;
    if (m_settings.RevalidateBuiltInFbcBeforeRun)
    {
        try
        {
            const auto identityBefore = ReadExternalConverterExecutableIdentity(
                m_settings.CommandProfile.ExecutablePath);
            const auto validation = ConverterValidation::ValidateBuiltInConverterExecutable(
                identityBefore.CanonicalPath,
                identityBefore);
            const auto identityAfter = ReadExternalConverterExecutableIdentity(
                identityBefore.CanonicalPath);
            if (!validation.IsValid || identityBefore != identityAfter)
            {
                return BuildFailedResult("Configured converter changed or failed validation before execution.");
            }
            executableIdentity = identityAfter;
        }
        catch (const std::exception& exception)
        {
            return BuildFailedResult(
                "Configured converter could not be revalidated before execution: "
                + std::string{exception.what()});
        }
    }

    InpxWebReader::ConverterCommand::SResolvedConverterCommand command =
        InpxWebReader::ConverterCommand::CConverterCommandBuilder::Build(
            m_settings.CommandProfile,
            request,
            commandOutputDirectory);
    if (executableIdentity.has_value())
    {
        command.ExecutablePath = executableIdentity->CanonicalPath;
    }

    InpxWebReader::Foundation::EnsureDirectory(command.ExpectedOutputDirectory);
    {
        std::error_code ec;
        std::filesystem::remove(command.ExpectedOutputPath, ec);
    }
    const std::set<std::filesystem::path> outputSnapshot = SnapshotDirectoryEntries(
        command.ExpectedOutputDirectory);

    InpxWebReader::Foundation::EnsureDirectory(processWorkingDirectory);

    progressSink.ReportValue(0, "Starting converter process");

    const SExternalConverterProcessResult processResult = m_processRunner->Run(
        SExternalConverterProcessRequest{
            .Command = command,
            .WorkingDirectory = processWorkingDirectory,
            .PollInterval = m_settings.PollInterval,
            .Timeout = m_settings.Timeout,
            .ExpectedExecutableIdentity = executableIdentity,
            .ProcessHooks = m_settings.ProcessHooks
        },
        progressSink,
        stopToken);

    if (processResult.WasCancelled)
    {
        CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
        return BuildCancelledResult();
    }

    if (processResult.WasTimedOut)
    {
        CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
        return BuildTimedOutResult();
    }

    if (processResult.ExitCode != 0)
    {
        CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
        return BuildFailedResult(
            "Converter process exited with code " + std::to_string(processResult.ExitCode) + ".");
    }

    std::filesystem::path resolvedOutputPath = command.ExpectedOutputPath;

    if (command.OutputMode == InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory)
    {
        SProducedOutputResolution producedOutput =
            ResolveSingleProducedFile(command.ExpectedOutputDirectory, outputSnapshot, command.ExpectedOutputPath);

        if (!producedOutput.Path.has_value())
        {
            CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
            return BuildFailedResult(
                producedOutput.UnsafeCandidate
                    ? "Converter process produced a symlink or non-regular EPUB output."
                    : "Converter process did not produce one unambiguous EPUB output.");
        }

        try
        {
            if (producedOutput.Path->lexically_normal() != command.ExpectedOutputPath.lexically_normal())
            {
                InpxWebReader::Foundation::EnsureDirectory(command.ExpectedOutputPath.parent_path());
                MoveGeneratedOutputFile(*producedOutput.Path, command.ExpectedOutputPath);
            }
            else
            {
                SealGeneratedOutputFile(command.ExpectedOutputPath);
            }
        }
        catch (const std::exception& exception)
        {
            CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
            return BuildFailedResult(
                "Converter output could not be validated for publication: "
                + std::string{exception.what()});
        }

        CleanupProducedEntries(
            command.ExpectedOutputDirectory,
            outputSnapshot,
            command.ExpectedOutputPath);
        resolvedOutputPath = command.ExpectedOutputPath;
    }
    else
    {
        try
        {
            SealGeneratedOutputFile(command.ExpectedOutputPath);
        }
        catch (const std::exception&)
        {
            CleanupProducedEntries(command.ExpectedOutputDirectory, outputSnapshot);
            return BuildFailedResult(
                "Converter process completed, but the expected output is missing, a symlink, or not a regular file.");
        }
        CleanupProducedEntries(
            command.ExpectedOutputDirectory,
            outputSnapshot,
            command.ExpectedOutputPath);
    }

    progressSink.ReportValue(100, "Converter process completed");

    return {
        .Status = InpxWebReader::Domain::EConversionStatus::Succeeded,
        .OutputPath = resolvedOutputPath
    };
}

} // namespace InpxWebReader::ConverterRuntime
