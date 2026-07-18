#include "Converter/BuiltInConverterCliValidation.hpp"

#include <algorithm>
#include <optional>
#include <vector>

#include "Converter/BuiltInConverterCliProbe.hpp"
#include "Foundation/StringUtils.hpp"

namespace InpxWebReader::ConverterValidation {
namespace {

[[nodiscard]] SBuiltInConverterCliValidationResult ValidateBuiltInConverterExecutableImpl(
    const std::filesystem::path& executablePath,
    const std::chrono::milliseconds timeout,
    const std::optional<InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity>&
        expectedIdentity)
{
    if (!IsSupportedBuiltInConverterExecutablePath(executablePath))
    {
        return {};
    }

    const std::optional<std::string> output = ProbeBuiltInConverterHelpOutput(
        executablePath,
        timeout,
        expectedIdentity);
    if (!output.has_value())
    {
        return {};
    }

    return ValidateBuiltInFbcHelpOutput(*output);
}

[[nodiscard]] std::vector<std::string> SplitLines(std::string_view value)
{
    std::vector<std::string> lines;
    std::size_t lineStart = 0;

    while (lineStart < value.size())
    {
        const std::size_t lineEnd = value.find('\n', lineStart);
        const std::size_t spanEnd = lineEnd == std::string_view::npos ? value.size() : lineEnd;
        std::string line(value.substr(lineStart, spanEnd - lineStart));
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }

        if (!line.empty())
        {
            lines.push_back(std::move(line));
        }

        if (lineEnd == std::string_view::npos)
        {
            break;
        }

        lineStart = lineEnd + 1;
    }

    return lines;
}

[[nodiscard]] std::string Trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isspace(character) != 0;
    });

    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char character)
    {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last)
    {
        return {};
    }

    return std::string(first, last);
}

} // namespace

bool IsSupportedBuiltInConverterExecutablePath(
    const std::filesystem::path& executablePath) noexcept
{
    std::error_code errorCode;
    const bool isRegularFile = std::filesystem::is_regular_file(executablePath, errorCode);
    if (errorCode || !isRegularFile)
    {
        return false;
    }

    const std::string fileName = Foundation::ToLowerAscii(executablePath.filename().string());
    return fileName == "fbc";
}

SBuiltInConverterCliValidationResult ValidateBuiltInConverterExecutable(
    const std::filesystem::path& executablePath,
    const std::chrono::milliseconds timeout)
{
    return ValidateBuiltInConverterExecutableImpl(executablePath, timeout, std::nullopt);
}

SBuiltInConverterCliValidationResult ValidateBuiltInConverterExecutable(
    const std::filesystem::path& executablePath,
    const InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity& expectedIdentity,
    const std::chrono::milliseconds timeout)
{
    return ValidateBuiltInConverterExecutableImpl(executablePath, timeout, expectedIdentity);
}

SBuiltInConverterCliValidationResult ValidateBuiltInFbcHelpOutput(
    const std::string_view output)
{
    const std::string normalized = Foundation::ToLowerAscii(std::string{output});
    const bool looksLikeFbc =
        normalized.find("usage:") != std::string::npos
        && normalized.find("fbc") != std::string::npos
        && normalized.find("fb2") != std::string::npos
        && normalized.find("convert") != std::string::npos;

    SBuiltInConverterCliValidationResult result = {
        .IsValid = looksLikeFbc
    };

    if (!looksLikeFbc)
    {
        return result;
    }

    const std::vector<std::string> lines = SplitLines(output);
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string normalizedLine = Foundation::ToLowerAscii(lines[index]);
        if (normalizedLine.starts_with("version:"))
        {
            const std::string inlineVersion = Trim(lines[index].substr(std::string("VERSION:").size()));
            if (!inlineVersion.empty())
            {
                result.VersionString = inlineVersion;
                return result;
            }

            if (index + 1 < lines.size())
            {
                result.VersionString = Trim(lines[index + 1]);
                return result;
            }
        }
    }

    return result;
}

} // namespace InpxWebReader::ConverterValidation
