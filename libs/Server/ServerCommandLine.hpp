#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace InpxWebReader::Server {

enum class EServerCommand
{
    Run,
    ShowHelp,
    ShowVersion
};

struct SServerCommandLine
{
    EServerCommand Command = EServerCommand::Run;
    std::optional<std::filesystem::path> ConfigPath = std::nullopt;
};

class CServerCommandLineParser final
{
public:
    [[nodiscard]] static SServerCommandLine Parse(const std::vector<std::string>& argumentsUtf8);
    [[nodiscard]] static std::string BuildHelpText();
};

} // namespace InpxWebReader::Server
