#include <catch2/catch_test_macros.hpp>

#include "Converter/ConverterConfiguration.hpp"

TEST_CASE("Disabled converter configuration builds no command profile", "[converter-config]")
{
    const InpxWebReader::ConverterConfiguration::SConverterConfiguration configuration;

    REQUIRE(configuration.IsValid());
    REQUIRE_FALSE(InpxWebReader::ConverterConfiguration::TryBuildCommandProfile(configuration).has_value());
}

TEST_CASE("Built-in fb2cng configuration produces the default command profile", "[converter-config]")
{
    const InpxWebReader::ConverterConfiguration::SConverterConfiguration configuration{
        .Mode = InpxWebReader::ConverterConfiguration::EConverterConfigurationMode::BuiltInFb2Cng,
        .Fb2Cng = {
            .ExecutablePath = "/opt/fbc"
        }
    };

    const auto profile = InpxWebReader::ConverterConfiguration::TryBuildCommandProfile(configuration);

    REQUIRE(configuration.IsValid());
    REQUIRE(profile.has_value());
    REQUIRE(profile->ExecutablePath == std::filesystem::path("/opt/fbc"));
    REQUIRE(profile->OutputMode == InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory);
    REQUIRE(profile->ArgumentTemplate == std::vector<std::string>({
        "convert",
        "--to",
        "{output_format}",
        "--overwrite",
        "{source}",
        "{destination_dir}"
    }));
}

TEST_CASE("Invalid enabled converter configuration is rejected", "[converter-config]")
{
    const InpxWebReader::ConverterConfiguration::SConverterConfiguration configuration{
        .Mode = InpxWebReader::ConverterConfiguration::EConverterConfigurationMode::BuiltInFb2Cng
    };

    REQUIRE_FALSE(configuration.IsValid());
    REQUIRE_FALSE(InpxWebReader::ConverterConfiguration::TryBuildCommandProfile(configuration).has_value());
}
