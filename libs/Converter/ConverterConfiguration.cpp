#include "Converter/ConverterConfiguration.hpp"

namespace InpxWebReader::ConverterConfiguration {

bool SConverterConfiguration::IsValid() const noexcept
{
    switch (Mode)
    {
    case EConverterConfigurationMode::Disabled:
        return true;
    case EConverterConfigurationMode::BuiltInFb2Cng:
        return Fb2Cng.IsValid();
    }

    return false;
}

std::optional<InpxWebReader::ConverterCommand::SConverterCommandProfile> TryBuildCommandProfile(
    const SConverterConfiguration& configuration)
{
    if (!configuration.IsValid())
    {
        return std::nullopt;
    }

    switch (configuration.Mode)
    {
    case EConverterConfigurationMode::Disabled:
        return std::nullopt;
    case EConverterConfigurationMode::BuiltInFb2Cng:
        return InpxWebReader::ConverterCommand::CConverterCommandBuilder::CreateFb2CngProfile(
            configuration.Fb2Cng.ExecutablePath);
    }

    return std::nullopt;
}

} // namespace InpxWebReader::ConverterConfiguration
