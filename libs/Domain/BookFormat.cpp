#include "Domain/BookFormat.hpp"

namespace InpxWebReader::Domain {

std::string_view ToString(const EBookFormat format) noexcept
{
    switch (format)
    {
    case EBookFormat::Epub:
        return "epub";
    case EBookFormat::Fb2:
        return "fb2";
    }

    return "unknown";
}

std::optional<EBookFormat> TryParseBookFormat(const std::string_view value) noexcept
{
    if (value == "epub" || value == ".epub" || value == "EPUB")
    {
        return EBookFormat::Epub;
    }

    if (value == "fb2" || value == ".fb2" || value == "FB2")
    {
        return EBookFormat::Fb2;
    }

    return std::nullopt;
}

} // namespace InpxWebReader::Domain
