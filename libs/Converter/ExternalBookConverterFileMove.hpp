#pragma once

#include <filesystem>

namespace InpxWebReader::ConverterRuntime {

[[nodiscard]] bool IsGeneratedOutputRegularFile(
    const std::filesystem::path& path) noexcept;

void SealGeneratedOutputFile(const std::filesystem::path& path);

void MoveGeneratedOutputFile(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath);

} // namespace InpxWebReader::ConverterRuntime
