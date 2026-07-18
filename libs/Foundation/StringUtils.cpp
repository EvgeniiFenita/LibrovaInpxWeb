#include "Foundation/StringUtils.hpp"

#include <algorithm>

namespace {

[[nodiscard]] constexpr char ToLowerAsciiCharacter(const unsigned char character) noexcept
{
    if (character >= static_cast<unsigned char>('A')
        && character <= static_cast<unsigned char>('Z'))
    {
        return static_cast<char>(character + static_cast<unsigned char>('a' - 'A'));
    }

    return static_cast<char>(character);
}

} // namespace

namespace InpxWebReader::Foundation {

std::string ToLowerAscii(std::string value)
{
    std::ranges::transform(value, value.begin(), ToLowerAsciiCharacter);
    return value;
}

bool EndsWithAsciiInsensitive(const std::string_view value, const std::string_view suffix) noexcept
{
    if (suffix.size() > value.size())
    {
        return false;
    }

    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index)
    {
        const auto left = static_cast<unsigned char>(value[offset + index]);
        const auto right = static_cast<unsigned char>(suffix[index]);
        if (ToLowerAsciiCharacter(left) != ToLowerAsciiCharacter(right))
        {
            return false;
        }
    }

    return true;
}

} // namespace InpxWebReader::Foundation
