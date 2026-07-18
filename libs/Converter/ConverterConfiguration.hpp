#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "Converter/ConverterCommandBuilder.hpp"

namespace InpxWebReader::ConverterConfiguration {

enum class EConverterConfigurationMode
{
    Disabled,
    BuiltInFb2Cng
};

struct SFb2CngConverterSettings
{
    std::filesystem::path ExecutablePath;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return !ExecutablePath.empty();
    }
};

struct SConverterConfiguration
{
    EConverterConfigurationMode Mode = EConverterConfigurationMode::Disabled;
    SFb2CngConverterSettings Fb2Cng;

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] std::optional<InpxWebReader::ConverterCommand::SConverterCommandProfile> TryBuildCommandProfile(
    const SConverterConfiguration& configuration);

} // namespace InpxWebReader::ConverterConfiguration
