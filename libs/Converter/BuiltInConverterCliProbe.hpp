#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "Converter/ExternalConverterProcessRunner.hpp"

namespace InpxWebReader::ConverterValidation {

struct SBuiltInConverterCliProbeHooks
{
    std::function<void()> BeforeExecutableOpen;
};

[[nodiscard]] std::optional<std::string> ProbeBuiltInConverterHelpOutput(
    const std::filesystem::path& executablePath,
    std::chrono::milliseconds timeout,
    const std::optional<InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity>&
        expectedIdentity = std::nullopt,
    const SBuiltInConverterCliProbeHooks& hooks = {});

} // namespace InpxWebReader::ConverterValidation
