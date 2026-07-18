#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace InpxWebReader::Unicode {

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view path);
[[nodiscard]] std::string CodePageToUtf8(
    std::string_view value,
    unsigned int codePage,
    std::string_view errorContext,
    const std::function<void()>& checkpoint = {});
[[nodiscard]] std::string DecodeLegacyCyrillicToUtf8(
    std::string_view value,
    std::string_view errorContext,
    const std::function<void()>& checkpoint = {});
[[nodiscard]] std::string Windows1251ToUtf8Lossy(
    std::string_view value,
    const std::function<void()>& checkpoint = {});
[[nodiscard]] bool IsValidUtf8(std::string_view value) noexcept;
[[nodiscard]] bool IsValidUtf8(
    std::string_view value,
    const std::function<void()>& checkpoint);
[[nodiscard]] std::optional<std::string> TryTrimTrailingIncompleteUtf8(
    std::string_view value,
    const std::function<void()>& checkpoint = {});
[[nodiscard]] std::optional<std::string> TryNormalizeUtf8WhitespaceAndCaseFold(std::string_view value);
[[nodiscard]] std::string Utf16LeToUtf8(
    const void* data,
    std::size_t byteCount,
    const std::function<void()>& checkpoint = {});
[[nodiscard]] std::string Utf16BeToUtf8(
    const void* data,
    std::size_t byteCount,
    const std::function<void()>& checkpoint = {});

} // namespace InpxWebReader::Unicode
