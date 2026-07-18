#include "Foundation/FilenamePolicy.hpp"

#include <string>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Foundation {
namespace {

void TrimTrailingFilenameWhitespace(std::string& valueUtf8)
{
    while (!valueUtf8.empty() && valueUtf8.back() == ' ')
    {
        valueUtf8.pop_back();
    }
}

} // namespace

std::string SanitizeDownloadFilenameBaseUtf8(
    std::string valueUtf8,
    const std::size_t maxBaseNameBytes,
    const std::string_view fallbackUtf8)
{
    for (char& ch : valueUtf8)
    {
        const auto byte = static_cast<unsigned char>(ch);
        if (ch == '\\'
            || ch == '/'
            || ch == ':'
            || ch == '*'
            || ch == '?'
            || ch == '"'
            || ch == '<'
            || ch == '>'
            || ch == '|'
            || byte < 0x20)
        {
            ch = '_';
        }
    }

    TrimTrailingFilenameWhitespace(valueUtf8);

    if (valueUtf8.empty())
    {
        return std::string{fallbackUtf8};
    }

    if (maxBaseNameBytes > 0 && valueUtf8.size() > maxBaseNameBytes)
    {
        valueUtf8.resize(maxBaseNameBytes);
        if (const auto trimmed = Unicode::TryTrimTrailingIncompleteUtf8(valueUtf8))
        {
            valueUtf8 = *trimmed;
        }
        TrimTrailingFilenameWhitespace(valueUtf8);
    }

    if (valueUtf8.empty())
    {
        return std::string{fallbackUtf8};
    }

    return valueUtf8;
}

} // namespace InpxWebReader::Foundation
