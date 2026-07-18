#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace InpxWebReader::Inpx {

struct SInpxSourceValidationResult
{
    bool IsValid = false;
    std::filesystem::path InpxPath;
    std::filesystem::path ArchiveRoot;
    std::string ErrorUtf8;
};

class CInpxSourceConfiguration final
{
public:
    [[nodiscard]] static SInpxSourceValidationResult Validate(
        const std::filesystem::path& inpxPath,
        std::optional<std::filesystem::path> archiveRootOverride = std::nullopt);
};

} // namespace InpxWebReader::Inpx
