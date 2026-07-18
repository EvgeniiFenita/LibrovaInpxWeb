#include "Converter/ConverterCommandBuilder.hpp"

#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ConverterCommand {
namespace {

std::string ToConverterFormat(const InpxWebReader::Domain::EBookFormat format)
{
    switch (format)
    {
    case InpxWebReader::Domain::EBookFormat::Epub:
        return "epub2";
    case InpxWebReader::Domain::EBookFormat::Fb2:
        return "fb2";
    }

    throw std::runtime_error("Unsupported destination format for converter command.");
}

std::string ExpandTemplate(
    const std::string_view argumentTemplate,
    const std::unordered_map<std::string, std::string>& replacements)
{
    std::string resolvedArgument;
    resolvedArgument.reserve(argumentTemplate.size());

    for (std::size_t index = 0; index < argumentTemplate.size(); ++index)
    {
        if (argumentTemplate[index] != '{')
        {
            resolvedArgument.push_back(argumentTemplate[index]);
            continue;
        }

        const std::size_t placeholderEnd = argumentTemplate.find('}', index);

        if (placeholderEnd == std::string_view::npos)
        {
            resolvedArgument.push_back(argumentTemplate[index]);
            continue;
        }

        const std::string placeholder(argumentTemplate.substr(index, placeholderEnd - index + 1));

        if (const auto replacementIterator = replacements.find(placeholder); replacementIterator != replacements.end())
        {
            resolvedArgument.append(replacementIterator->second);
        }
        else
        {
            resolvedArgument.append(placeholder);
        }

        index = placeholderEnd;
    }

    return resolvedArgument;
}

} // namespace

SConverterCommandProfile CConverterCommandBuilder::CreateFb2CngProfile(
    const std::filesystem::path& executablePath)
{
    if (executablePath.empty())
    {
        throw std::invalid_argument("Converter executable path must not be empty.");
    }

    SConverterCommandProfile profile = {
        .ExecutablePath = executablePath,
        .OutputMode = EConverterOutputMode::SingleFileInDestinationDirectory
    };

    profile.ArgumentTemplate.push_back("convert");
    profile.ArgumentTemplate.push_back("--to");
    profile.ArgumentTemplate.push_back("{output_format}");
    profile.ArgumentTemplate.push_back("--overwrite");
    profile.ArgumentTemplate.push_back("{source}");
    profile.ArgumentTemplate.push_back("{destination_dir}");

    return profile;
}

SResolvedConverterCommand CConverterCommandBuilder::Build(
    const SConverterCommandProfile& profile,
    const InpxWebReader::Domain::SConversionRequest& request,
    const std::optional<std::filesystem::path>& outputDirectoryOverride)
{
    if (!profile.IsValid())
    {
        throw std::invalid_argument("Converter command profile must contain an executable and at least one argument.");
    }

    if (!request.IsValid())
    {
        throw std::invalid_argument("Conversion request must contain valid source and destination paths.");
    }

    const std::filesystem::path outputDirectory =
        outputDirectoryOverride.value_or(request.DestinationPath.parent_path());

    const std::unordered_map<std::string, std::string> replacements{
        {"{source}", InpxWebReader::Unicode::PathToUtf8(request.SourcePath)},
        {"{destination}", InpxWebReader::Unicode::PathToUtf8(request.DestinationPath)},
        {"{destination_dir}", InpxWebReader::Unicode::PathToUtf8(outputDirectory)},
        {"{output_format}", ToConverterFormat(request.DestinationFormat)}
    };

    SResolvedConverterCommand command = {
        .ExecutablePath = profile.ExecutablePath,
        .OutputMode = profile.OutputMode,
        .ExpectedOutputPath = request.DestinationPath,
        .ExpectedOutputDirectory = outputDirectory
    };

    command.Arguments.reserve(profile.ArgumentTemplate.size());

    for (const std::string& argumentTemplate : profile.ArgumentTemplate)
    {
        command.Arguments.push_back(ExpandTemplate(argumentTemplate, replacements));
    }

    return command;
}

} // namespace InpxWebReader::ConverterCommand
