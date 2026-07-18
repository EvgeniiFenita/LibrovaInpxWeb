#include "Domain/MetadataNormalization.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Domain {
namespace {

constexpr std::string_view InvalidUtf8Sentinel = "\xEE\x80\x80";
constexpr std::string_view InvalidUtf8Prefix = "invalid-utf8:";

[[nodiscard]] std::string EscapeValidTextForNormalizationKey(const std::string& normalized)
{
    if (!normalized.starts_with(InvalidUtf8Sentinel))
    {
        return normalized;
    }

    std::string escaped;
    escaped.reserve(InvalidUtf8Sentinel.size() + normalized.size());
    escaped.append(InvalidUtf8Sentinel);
    escaped.append(normalized);
    return escaped;
}

[[nodiscard]] std::string EncodeInvalidUtf8ForNormalization(const std::string_view value)
{
    constexpr char HexDigits[] = "0123456789abcdef";

    std::string encoded;
    encoded.reserve(InvalidUtf8Sentinel.size() + InvalidUtf8Prefix.size() + value.size() * 2);
    encoded.append(InvalidUtf8Sentinel);
    encoded.append(InvalidUtf8Prefix);

    for (const unsigned char byte : value)
    {
        encoded.push_back(HexDigits[byte >> 4]);
        encoded.push_back(HexDigits[byte & 0x0F]);
    }

    return encoded;
}

[[nodiscard]] bool IsValidIsbn10Checksum(const std::string_view value) noexcept
{
    if (value.size() != 10)
    {
        return false;
    }

    int sum = 0;
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        int digit = 0;
        if (index == value.size() - 1 && value[index] == 'X')
        {
            digit = 10;
        }
        else if (std::isdigit(static_cast<unsigned char>(value[index])) != 0)
        {
            digit = value[index] - '0';
        }
        else
        {
            return false;
        }

        sum += digit * static_cast<int>(10 - index);
    }

    return (sum % 11) == 0;
}

[[nodiscard]] bool IsValidIsbn13Checksum(const std::string_view value) noexcept
{
    if (value.size() != 13
        || !std::ranges::all_of(value, [](const char current) {
            return std::isdigit(static_cast<unsigned char>(current)) != 0;
        }))
    {
        return false;
    }

    int weightedSum = 0;
    for (std::size_t index = 0; index < 12; ++index)
    {
        const int digit = value[index] - '0';
        weightedSum += digit * ((index % 2) == 0 ? 1 : 3);
    }

    const int expectedCheckDigit = (10 - (weightedSum % 10)) % 10;
    return expectedCheckDigit == (value[12] - '0');
}

} // namespace

std::string NormalizeText(const std::string_view value)
{
    if (const std::optional<std::string> normalized = InpxWebReader::Unicode::TryNormalizeUtf8WhitespaceAndCaseFold(value);
        normalized.has_value())
    {
        return EscapeValidTextForNormalizationKey(*normalized);
    }

    return EncodeInvalidUtf8ForNormalization(value);
}

std::optional<std::string> NormalizeIsbn(const std::optional<std::string>& value)
{
    if (!value.has_value())
    {
        return std::nullopt;
    }

    if (!InpxWebReader::Unicode::IsValidUtf8(*value))
    {
        return std::nullopt;
    }

    std::string normalized;
    normalized.reserve(value->size());

    for (const unsigned char currentByte : *value)
    {
        if (std::isdigit(currentByte))
        {
            normalized.push_back(static_cast<char>(currentByte));
            continue;
        }

        if (currentByte == 'x' || currentByte == 'X')
        {
            normalized.push_back('X');
        }
    }

    if (normalized.size() == 10)
    {
        return IsValidIsbn10Checksum(normalized) ? std::make_optional(normalized) : std::nullopt;
    }

    if (normalized.size() == 13)
    {
        return IsValidIsbn13Checksum(normalized) ? std::make_optional(normalized) : std::nullopt;
    }

    return std::nullopt;
}

} // namespace InpxWebReader::Domain
