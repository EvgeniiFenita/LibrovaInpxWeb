#include <catch2/catch_test_macros.hpp>

#include "Converter/ConverterCommandBuilder.hpp"

TEST_CASE("Converter command builder creates default fb2cng command profile", "[converter-command]")
{
    const auto profile = InpxWebReader::ConverterCommand::CConverterCommandBuilder::CreateFb2CngProfile("/opt/fbc");

    REQUIRE(profile.IsValid());
    REQUIRE(profile.ExecutablePath == std::filesystem::path{"/opt/fbc"});
    REQUIRE(profile.OutputMode == InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory);
    REQUIRE(profile.ArgumentTemplate == std::vector<std::string>({
        "convert",
        "--to",
        "{output_format}",
        "--overwrite",
        "{source}",
        "{destination_dir}"
    }));
}

TEST_CASE("Converter command builder resolves placeholders for generic templates", "[converter-command]")
{
    const InpxWebReader::ConverterCommand::SConverterCommandProfile profile{
        .ExecutablePath = "/opt/custom-converter",
        .ArgumentTemplate = {"--input", "{source}", "--output", "{destination}", "--format", "{output_format}"},
        .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath
    };

    const InpxWebReader::Domain::SConversionRequest request{
        .SourcePath = "ScanSupport/source.fb2",
        .DestinationPath = "Temp/book.epub",
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    };

    const auto command = InpxWebReader::ConverterCommand::CConverterCommandBuilder::Build(profile, request);

    REQUIRE(command.IsValid());
    REQUIRE(command.ExecutablePath == std::filesystem::path{"/opt/custom-converter"});
    REQUIRE(command.Arguments == std::vector<std::string>({
        "--input",
        "ScanSupport/source.fb2",
        "--output",
        "Temp/book.epub",
        "--format",
        "epub2"
    }));
    REQUIRE(command.ExpectedOutputPath == std::filesystem::path{"Temp/book.epub"});
    REQUIRE(command.ExpectedOutputDirectory == std::filesystem::path{"Temp"});
}

TEST_CASE("Converter command builder resolves fb2cng output directory contract", "[converter-command]")
{
    const auto profile = InpxWebReader::ConverterCommand::CConverterCommandBuilder::CreateFb2CngProfile("/opt/fbc");
    const InpxWebReader::Domain::SConversionRequest request{
        .SourcePath = "ScanSupport/book.fb2",
        .DestinationPath = "Temp/converted/book.epub",
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    };

    const auto command = InpxWebReader::ConverterCommand::CConverterCommandBuilder::Build(profile, request);

    REQUIRE(command.OutputMode == InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory);
    REQUIRE(command.Arguments == std::vector<std::string>({
        "convert",
        "--to",
        "epub2",
        "--overwrite",
        "ScanSupport/book.fb2",
        "Temp/converted"
    }));
    REQUIRE(command.ExpectedOutputPath == std::filesystem::path{"Temp/converted/book.epub"});
}

TEST_CASE("Converter command builder preserves literal placeholder text inside replacement paths", "[converter-command]")
{
    const InpxWebReader::ConverterCommand::SConverterCommandProfile profile{
        .ExecutablePath = "/opt/custom-converter",
        .ArgumentTemplate = {"--input", "{source}", "--output", "{destination}", "--dir", "{destination_dir}"},
        .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath
    };

    const InpxWebReader::Domain::SConversionRequest request{
        .SourcePath = std::filesystem::path{u8"ScanSupport/{output_format}/source.fb2"},
        .DestinationPath = std::filesystem::path{u8"Temp/{output_format}/book.epub"},
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    };

    const auto command = InpxWebReader::ConverterCommand::CConverterCommandBuilder::Build(profile, request);

    REQUIRE(command.Arguments == std::vector<std::string>({
        "--input",
        "ScanSupport/{output_format}/source.fb2",
        "--output",
        "Temp/{output_format}/book.epub",
        "--dir",
        "Temp/{output_format}"
    }));
}
