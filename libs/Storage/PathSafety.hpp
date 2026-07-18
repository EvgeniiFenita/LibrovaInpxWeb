#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace InpxWebReader::SafePaths {

[[nodiscard]] bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate);

[[nodiscard]] std::optional<std::filesystem::path>
TryResolvePathWithinRoot(const std::filesystem::path& root,
                         const std::filesystem::path& path,
                         std::string_view unsafePathMessage,
                         std::string_view canonicalizationErrorMessage);

[[nodiscard]] std::optional<std::filesystem::path>
TryResolvePathWithinCanonicalRoot(const std::filesystem::path& canonicalRoot,
                                  const std::filesystem::path& path,
                                  std::string_view unsafePathMessage,
                                  std::string_view canonicalizationErrorMessage);

} // namespace InpxWebReader::SafePaths
