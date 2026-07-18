#include "Server/ServerCommandLine.hpp"

#include <stdexcept>
#include <string_view>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Server {
namespace {

[[nodiscard]] bool IsHelpFlag(const std::string_view value) noexcept
{
    return value == "--help" || value == "-h";
}

[[nodiscard]] bool IsVersionFlag(const std::string_view value) noexcept
{
    return value == "--version" || value == "-v";
}

} // namespace

SServerCommandLine CServerCommandLineParser::Parse(const std::vector<std::string>& argumentsUtf8)
{
    SServerCommandLine result;

    for (std::size_t index = 0; index < argumentsUtf8.size(); ++index)
    {
        const std::string_view argument = argumentsUtf8[index];
        if (IsHelpFlag(argument))
        {
            result.Command = EServerCommand::ShowHelp;
            return result;
        }

        if (IsVersionFlag(argument))
        {
            result.Command = EServerCommand::ShowVersion;
            return result;
        }

        if (argument == "--config")
        {
            if (result.ConfigPath.has_value())
            {
                throw std::runtime_error("Duplicate --config option.");
            }
            if (index + 1 >= argumentsUtf8.size())
            {
                throw std::runtime_error("Missing value for --config.");
            }

            result.ConfigPath = Unicode::PathFromUtf8(argumentsUtf8[++index]);
            continue;
        }

        throw std::runtime_error("Unknown server command line option: " + std::string{argument});
    }

    if (!result.ConfigPath.has_value())
    {
        throw std::runtime_error("Missing required --config <path> option.");
    }

    return result;
}

std::string CServerCommandLineParser::BuildHelpText()
{
    return
        "Usage: inpx-web-reader --config <path>\n"
        "\n"
        "Options:\n"
        "  --config <path>   Path to the InpxWebReader server JSON config.\n"
        "  --help, -h        Show this help text and exit before backend startup.\n"
        "  --version, -v     Show the product version and exit before backend startup.\n";
}

} // namespace InpxWebReader::Server
