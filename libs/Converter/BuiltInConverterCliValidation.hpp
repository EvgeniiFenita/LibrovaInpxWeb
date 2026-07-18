#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "Converter/ExternalConverterProcessRunner.hpp"

namespace InpxWebReader::ConverterValidation {

struct SBuiltInConverterCliValidationResult
{
    bool IsValid = false;
    std::string VersionString;
};

[[nodiscard]] bool IsSupportedBuiltInConverterExecutablePath(
    const std::filesystem::path& executablePath) noexcept;

[[nodiscard]] SBuiltInConverterCliValidationResult ValidateBuiltInConverterExecutable(
    const std::filesystem::path& executablePath,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

[[nodiscard]] SBuiltInConverterCliValidationResult ValidateBuiltInConverterExecutable(
    const std::filesystem::path& executablePath,
    const InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity& expectedIdentity,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

[[nodiscard]] SBuiltInConverterCliValidationResult ValidateBuiltInFbcHelpOutput(
    std::string_view output);

} // namespace InpxWebReader::ConverterValidation
