#include "Parsing/Fb2Parser.hpp"
#include "Parsing/Fb2GenreMapper.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "Foundation/BookPayloadLimits.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Fb2Parsing {
namespace {

constexpr std::size_t GCancellationCheckpointIntervalBytes = 64ull * 1024ull;

void RunCheckpoint(const std::function<void()>& checkpoint)
{
    if (checkpoint)
    {
        checkpoint();
    }
}

void ThrowIfCancelled(const std::stop_token stopToken)
{
    if (stopToken.stop_requested())
    {
        throw std::runtime_error("FB2 parsing cancelled.");
    }
}

enum class EDeclaredXmlEncoding
{
    Unknown,
    Utf8,
    Windows1251,
    Cp866,
    Utf16LittleEndian,
    Utf16BigEndian,
    Utf16
};

struct SXmlEncodingDeclaration
{
    EDeclaredXmlEncoding Encoding = EDeclaredXmlEncoding::Unknown;
    std::size_t ValueOffset = 0;
    std::size_t ValueLength = 0;
};

[[nodiscard]] EDeclaredXmlEncoding ParseEncodingName(const std::string_view name) noexcept
{
    constexpr std::array aliases = {
        std::pair{std::string_view{"utf-8"}, EDeclaredXmlEncoding::Utf8},
        std::pair{std::string_view{"windows-1251"}, EDeclaredXmlEncoding::Windows1251},
        std::pair{std::string_view{"cp1251"}, EDeclaredXmlEncoding::Windows1251},
        std::pair{std::string_view{"cp866"}, EDeclaredXmlEncoding::Cp866},
        std::pair{std::string_view{"ibm866"}, EDeclaredXmlEncoding::Cp866},
        std::pair{std::string_view{"utf-16le"}, EDeclaredXmlEncoding::Utf16LittleEndian},
        std::pair{std::string_view{"utf-16be"}, EDeclaredXmlEncoding::Utf16BigEndian},
        std::pair{std::string_view{"utf-16"}, EDeclaredXmlEncoding::Utf16}
    };
    for (const auto& [alias, encoding] : aliases)
    {
        if (name == alias)
        {
            return encoding;
        }
    }
    return EDeclaredXmlEncoding::Unknown;
}

[[nodiscard]] std::optional<SXmlEncodingDeclaration> TryParseEncodingDeclaration(const std::string_view text)
{
    const std::size_t declarationEnd = text.find("?>");
    if (declarationEnd == std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::string lowerDeclaration = Foundation::ToLowerAscii(
        std::string{text.substr(0, declarationEnd)});
    std::size_t position = lowerDeclaration.find("encoding");
    if (position == std::string::npos)
    {
        return std::nullopt;
    }
    position += std::string_view{"encoding"}.size();

    const auto skipWhitespace = [&lowerDeclaration, declarationEnd](std::size_t& offset) {
        while (offset < declarationEnd
            && std::isspace(static_cast<unsigned char>(lowerDeclaration[offset])) != 0)
        {
            ++offset;
        }
    };
    skipWhitespace(position);
    if (position >= declarationEnd || lowerDeclaration[position] != '=')
    {
        return std::nullopt;
    }
    ++position;
    skipWhitespace(position);
    if (position >= declarationEnd
        || (lowerDeclaration[position] != '\'' && lowerDeclaration[position] != '"'))
    {
        return std::nullopt;
    }

    const char quote = lowerDeclaration[position++];
    const std::size_t valueEnd = lowerDeclaration.find(quote, position);
    if (valueEnd == std::string::npos || valueEnd > declarationEnd)
    {
        return std::nullopt;
    }

    return SXmlEncodingDeclaration{
        .Encoding = ParseEncodingName(
            std::string_view{lowerDeclaration}.substr(position, valueEnd - position)),
        .ValueOffset = position,
        .ValueLength = valueEnd - position
    };
}

std::string ReplaceEncodingDeclaration(std::string text)
{
    const auto declaration = TryParseEncodingDeclaration(text);
    if (declaration.has_value() && declaration->Encoding != EDeclaredXmlEncoding::Unknown)
    {
        text.replace(declaration->ValueOffset, declaration->ValueLength, "utf-8");
    }
    return text;
}

[[nodiscard]] std::string Trim(const std::string_view value);

constexpr std::size_t GDiagnosticPreviewMaxBytes = 320;
constexpr std::size_t GMaxXmlOpenTagBytes = 64ull * 1024ull;

[[nodiscard]] std::string TruncateUtf8(std::string_view value, const std::size_t maxBytes)
{
    if (value.size() <= maxBytes)
    {
        return std::string{value};
    }

    std::size_t index = 0;
    std::size_t lastValidEnd = 0;

    while (index < value.size() && index < maxBytes)
    {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        std::size_t sequenceLength = 1;

        if ((lead & 0x80u) == 0)
        {
            sequenceLength = 1;
        }
        else if ((lead & 0xE0u) == 0xC0u)
        {
            sequenceLength = 2;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            sequenceLength = 3;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            sequenceLength = 4;
        }
        else
        {
            break;
        }

        if (index + sequenceLength > value.size() || index + sequenceLength > maxBytes)
        {
            break;
        }

        bool hasValidContinuationBytes = true;
        for (std::size_t offset = 1; offset < sequenceLength; ++offset)
        {
            const unsigned char continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0u) != 0x80u)
            {
                hasValidContinuationBytes = false;
                break;
            }
        }

        if (!hasValidContinuationBytes)
        {
            break;
        }

        lastValidEnd = index + sequenceLength;
        index += sequenceLength;
    }

    if (lastValidEnd == 0)
    {
        return {};
    }

    return std::string{value.substr(0, lastValidEnd)};
}

class CCompactPreviewBuilder final
{
public:
    CCompactPreviewBuilder()
    {
        m_text.reserve(GDiagnosticPreviewMaxBytes + 1);
    }

    [[nodiscard]] bool Append(const std::string_view text)
    {
        for (const char value : text)
        {
            if (std::isspace(static_cast<unsigned char>(value)) != 0)
            {
                m_hasPendingWhitespace = !m_text.empty();
                continue;
            }

            if (m_hasPendingWhitespace)
            {
                m_text.push_back(' ');
                m_hasPendingWhitespace = false;
                if (m_text.size() > GDiagnosticPreviewMaxBytes)
                {
                    return false;
                }
            }

            m_text.push_back(value);
            if (m_text.size() > GDiagnosticPreviewMaxBytes)
            {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] std::string Finish() const
    {
        if (m_text.empty())
        {
            return "<empty>";
        }

        if (m_text.size() <= GDiagnosticPreviewMaxBytes)
        {
            return m_text;
        }

        std::string result = TruncateUtf8(m_text, GDiagnosticPreviewMaxBytes);
        result.append("...");
        return result;
    }

private:
    std::string m_text;
    bool m_hasPendingWhitespace = false;
};

[[nodiscard]] std::string CompactPreview(const std::string_view text)
{
    CCompactPreviewBuilder builder;
    static_cast<void>(builder.Append(text));
    return builder.Finish();
}

class CPreviewLimitReached final
{
};

class CCompactPreviewXmlWriter final : public pugi::xml_writer
{
public:
    void write(const void* data, const std::size_t size) override
    {
        if (size == 0)
        {
            return;
        }
        if (!m_builder.Append(std::string_view{static_cast<const char*>(data), size}))
        {
            throw CPreviewLimitReached{};
        }
    }

    [[nodiscard]] std::string Finish() const
    {
        return m_builder.Finish();
    }

private:
    CCompactPreviewBuilder m_builder;
};

[[nodiscard]] std::string DecodeFb2Text(
    std::string rawBytes,
    const std::string_view sourceLabel,
    std::size_t& parserWarningCount,
    const std::stop_token stopToken)
{
    const auto checkpoint = [stopToken]() {
        ThrowIfCancelled(stopToken);
    };
    const std::string labelUtf8 = std::string{sourceLabel};

    // Step 1 — strip UTF-8 BOM (EF BB BF) so pugixml does not choke on the
    // XML declaration ("Error parsing document declaration" on BOM files).
    if (rawBytes.size() >= 3
        && static_cast<unsigned char>(rawBytes[0]) == 0xEFu
        && static_cast<unsigned char>(rawBytes[1]) == 0xBBu
        && static_cast<unsigned char>(rawBytes[2]) == 0xBFu)
    {
        rawBytes.erase(0, 3);
        InpxWebReader::Logging::DebugIfInitialized("FB2 UTF-8 BOM stripped: {}", labelUtf8);
    }

    // Step 1b — UTF-16 LE (BOM: FF FE): files from tools that save FB2 in UTF-16.
    if (rawBytes.size() >= 2
        && static_cast<unsigned char>(rawBytes[0]) == 0xFFu
        && static_cast<unsigned char>(rawBytes[1]) == 0xFEu)
    {
        InpxWebReader::Logging::InfoIfInitialized(
            "FB2 file is UTF-16 LE — converting to UTF-8: {}",
            labelUtf8);
        return ReplaceEncodingDeclaration(InpxWebReader::Unicode::Utf16LeToUtf8(
            rawBytes.data(),
            rawBytes.size(),
            checkpoint));
    }

    // Step 1c — UTF-16 BE (BOM: FE FF).
    if (rawBytes.size() >= 2
        && static_cast<unsigned char>(rawBytes[0]) == 0xFEu
        && static_cast<unsigned char>(rawBytes[1]) == 0xFFu)
    {
        InpxWebReader::Logging::InfoIfInitialized(
            "FB2 file is UTF-16 BE — converting to UTF-8: {}",
            labelUtf8);
        return ReplaceEncodingDeclaration(InpxWebReader::Unicode::Utf16BeToUtf8(
            rawBytes.data(),
            rawBytes.size(),
            checkpoint));
    }

    const auto prefixLength = std::min<std::size_t>(rawBytes.size(), 512);
    const auto declaredEncoding = TryParseEncodingDeclaration(
        std::string_view{rawBytes}.substr(0, prefixLength));

    // Step 2 — explicit CP1251 declaration: file says encoding="windows-1251" or "cp1251".
    if (declaredEncoding.has_value()
        && declaredEncoding->Encoding == EDeclaredXmlEncoding::Windows1251)
    {
        InpxWebReader::Logging::DebugIfInitialized(
            "FB2 explicit CP1251 declaration — converting to UTF-8: {}",
            labelUtf8);
        std::string utf8Text;
        try
        {
            utf8Text = InpxWebReader::Unicode::CodePageToUtf8(
                rawBytes,
                1251,
                "Failed to decode FB2 file from windows-1251.",
                checkpoint);
        }
        catch (const std::runtime_error&)
        {
            ThrowIfCancelled(stopToken);
            ++parserWarningCount;
            InpxWebReader::Logging::WarnIfInitialized(
                "FB2 explicit CP1251 decode failed - applying lossy CP1251 fallback: {}",
                labelUtf8);
            utf8Text = InpxWebReader::Unicode::Windows1251ToUtf8Lossy(
                rawBytes,
                checkpoint);
        }
        return ReplaceEncodingDeclaration(std::move(utf8Text));
    }

    if (declaredEncoding.has_value()
        && declaredEncoding->Encoding == EDeclaredXmlEncoding::Cp866)
    {
        InpxWebReader::Logging::DebugIfInitialized(
            "FB2 explicit CP866 declaration — converting to UTF-8: {}",
            labelUtf8);
        return ReplaceEncodingDeclaration(InpxWebReader::Unicode::CodePageToUtf8(
            rawBytes,
            866,
            "Failed to decode FB2 file from CP866.",
            checkpoint));
    }

    // Step 3 — legacy Cyrillic fallback: no explicit declaration but the byte stream is
    // not valid UTF-8 (misdeclared or encoding-less files from older tools).
    if (!InpxWebReader::Unicode::IsValidUtf8(rawBytes, checkpoint))
    {
        if (std::optional<std::string> trimmedText =
                InpxWebReader::Unicode::TryTrimTrailingIncompleteUtf8(rawBytes, checkpoint))
        {
            ++parserWarningCount;
            InpxWebReader::Logging::WarnIfInitialized(
                "FB2 file ends with an incomplete UTF-8 sequence — trimming trailing bytes: {}",
                labelUtf8);
            std::string text = std::move(*trimmedText);
            return text;
        }

        ++parserWarningCount;
        InpxWebReader::Logging::WarnIfInitialized(
            "FB2 file is not valid UTF-8 and has no supported encoding declaration — "
            "detecting CP1251 or CP866: {}",
            labelUtf8);
        std::string utf8Text = InpxWebReader::Unicode::DecodeLegacyCyrillicToUtf8(
            rawBytes,
            "Failed to decode misdeclared FB2 file from CP1251 or CP866.",
            checkpoint);
        return ReplaceEncodingDeclaration(std::move(utf8Text));
    }

    return rawBytes;
}

[[nodiscard]] bool MatchesLocalName(const std::string_view qualifiedName, const std::string_view localName)
{
    const std::size_t separatorIndex = qualifiedName.find(':');
    const std::string_view currentLocalName = separatorIndex == std::string_view::npos
        ? qualifiedName
        : qualifiedName.substr(separatorIndex + 1);
    return currentLocalName == localName;
}

[[nodiscard]] bool IsXmlNameChar(const char value)
{
    const unsigned char ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) || value == '_' || value == '-' || value == '.' || value == ':';
}

[[nodiscard]] std::size_t FindTextWithCheckpoints(
    const std::string_view text,
    const std::string_view needle,
    const std::size_t startOffset,
    const std::function<void()>& checkpoint)
{
    if (needle.empty())
    {
        return (std::min)(startOffset, text.size());
    }
    if (startOffset > text.size() || needle.size() > text.size() - startOffset)
    {
        return std::string_view::npos;
    }

    std::size_t nextCheckpoint = startOffset + GCancellationCheckpointIntervalBytes;
    const std::size_t lastStart = text.size() - needle.size();
    for (std::size_t index = startOffset; index <= lastStart; ++index)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }
        if (text.compare(index, needle.size(), needle) == 0)
        {
            return index;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] std::optional<std::size_t> FindXmlDeclarationEnd(
    const std::string_view text,
    const std::size_t startOffset,
    const std::function<void()>& checkpoint)
{
    bool inSingleQuotedValue = false;
    bool inDoubleQuotedValue = false;
    std::size_t bracketDepth = 0;

    std::size_t nextCheckpoint = startOffset;
    for (std::size_t index = startOffset; index < text.size(); ++index)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }
        const char value = text[index];
        if (value == '\'' && !inDoubleQuotedValue)
        {
            inSingleQuotedValue = !inSingleQuotedValue;
            continue;
        }

        if (value == '"' && !inSingleQuotedValue)
        {
            inDoubleQuotedValue = !inDoubleQuotedValue;
            continue;
        }

        if (inSingleQuotedValue || inDoubleQuotedValue)
        {
            continue;
        }

        if (value == '[')
        {
            ++bracketDepth;
            continue;
        }

        if (value == ']' && bracketDepth > 0)
        {
            --bracketDepth;
            continue;
        }

        if (value == '>' && bracketDepth == 0)
        {
            return index + 1;
        }
    }

    return std::nullopt;
}

[[nodiscard]] std::size_t FindXmlTagEnd(
    const std::string_view text,
    const std::size_t startOffset,
    const std::function<void()>& checkpoint)
{
    bool inSingleQuotedValue = false;
    bool inDoubleQuotedValue = false;
    std::size_t nextCheckpoint = startOffset;

    for (std::size_t index = startOffset; index < text.size(); ++index)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }

        const char value = text[index];
        if (value == '\'' && !inDoubleQuotedValue)
        {
            inSingleQuotedValue = !inSingleQuotedValue;
            continue;
        }

        if (value == '"' && !inSingleQuotedValue)
        {
            inDoubleQuotedValue = !inDoubleQuotedValue;
            continue;
        }

        if (value == '>' && !inSingleQuotedValue && !inDoubleQuotedValue)
        {
            return index;
        }
    }

    return std::string_view::npos;
}

struct SXmlTagRange
{
    std::size_t Start = std::string_view::npos;
    std::size_t NameStart = std::string_view::npos;
    std::size_t NameEnd = std::string_view::npos;
    std::size_t End = std::string_view::npos;
};

[[nodiscard]] std::optional<SXmlTagRange> FindNextTagRange(
    const std::string_view text,
    const std::size_t startOffset,
    const bool closingTag,
    const std::optional<std::string_view> localName = std::nullopt,
    const std::function<void()>& checkpoint = {})
{
    std::size_t current = startOffset;
    std::size_t nextCheckpoint = startOffset;

    while (current < text.size())
    {
        if (current >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = current + GCancellationCheckpointIntervalBytes;
        }
        const std::size_t tagStart = FindTextWithCheckpoints(text, "<", current, checkpoint);
        if (tagStart == std::string_view::npos || tagStart + 1 >= text.size())
        {
            return std::nullopt;
        }

        if (text.substr(tagStart, 4) == "<!--")
        {
            const std::size_t commentEnd = FindTextWithCheckpoints(
                text,
                "-->",
                tagStart + 4,
                checkpoint);
            if (commentEnd == std::string_view::npos)
            {
                return std::nullopt;
            }

            current = commentEnd + 3;
            continue;
        }

        if (text.substr(tagStart, 9) == "<![CDATA[")
        {
            const std::size_t cdataEnd = FindTextWithCheckpoints(
                text,
                "]]>",
                tagStart + 9,
                checkpoint);
            if (cdataEnd == std::string_view::npos)
            {
                return std::nullopt;
            }

            current = cdataEnd + 3;
            continue;
        }

        if (text.substr(tagStart, 2) == "<?")
        {
            const std::size_t processingInstructionEnd = FindTextWithCheckpoints(
                text,
                "?>",
                tagStart + 2,
                checkpoint);
            if (processingInstructionEnd == std::string_view::npos)
            {
                return std::nullopt;
            }

            current = processingInstructionEnd + 2;
            continue;
        }

        if (text.substr(tagStart, 2) == "<!")
        {
            const std::optional<std::size_t> declarationEnd = FindXmlDeclarationEnd(
                text,
                tagStart + 2,
                checkpoint);
            if (!declarationEnd.has_value())
            {
                return std::nullopt;
            }

            current = *declarationEnd;
            continue;
        }

        const char marker = text[tagStart + 1];
        if ((!closingTag && (marker == '/' || marker == '?' || marker == '!'))
            || (closingTag && marker != '/'))
        {
            current = tagStart + 1;
            continue;
        }

        const std::size_t nameStart = tagStart + (closingTag ? 2 : 1);
        std::size_t nameEnd = nameStart;
        std::size_t nextNameCheckpoint = nameStart;
        while (nameEnd < text.size() && IsXmlNameChar(text[nameEnd]))
        {
            if (nameEnd >= nextNameCheckpoint)
            {
                RunCheckpoint(checkpoint);
                nextNameCheckpoint = nameEnd + GCancellationCheckpointIntervalBytes;
            }
            ++nameEnd;
        }

        if (nameEnd == nameStart)
        {
            current = tagStart + 1;
            continue;
        }

        const std::string_view qualifiedName = text.substr(nameStart, nameEnd - nameStart);
        if (localName.has_value() && !MatchesLocalName(qualifiedName, *localName))
        {
            current = tagStart + 1;
            continue;
        }

        const std::size_t tagEnd = FindXmlTagEnd(text, nameEnd, checkpoint);
        if (tagEnd == std::string_view::npos)
        {
            return std::nullopt;
        }

        return SXmlTagRange{
            .Start = tagStart,
            .NameStart = nameStart,
            .NameEnd = nameEnd,
            .End = tagEnd
        };
    }

    return std::nullopt;
}

struct SDescriptionRange
{
    SXmlTagRange RootOpenTag;
    SXmlTagRange DescriptionOpenTag;
    SXmlTagRange DescriptionCloseTag;
};

[[nodiscard]] std::optional<SDescriptionRange> FindDescriptionRange(
    std::string_view text,
    const std::function<void()>& checkpoint = {});

struct SCatalogXmlDocument
{
    pugi::xml_document Document;
    std::size_t BinarySearchOffset = 0;
};

struct SMetadataXmlShape
{
    std::uint64_t Bytes = 0;
    std::uint64_t Nodes = 0;
    std::uint64_t Attributes = 0;
};

struct SCatalogXmlAnalysis
{
    SMetadataXmlShape Shape;
    std::optional<SDescriptionRange> DescriptionRange = std::nullopt;
};

void AddXmlFragmentShape(
    SMetadataXmlShape& shape,
    const std::string_view fragment,
    const std::function<void()>& checkpoint)
{
    shape.Bytes += static_cast<std::uint64_t>(fragment.size());
    bool insideTag = false;
    char quote = '\0';

    std::size_t index = 0;
    for (const char value : fragment)
    {
        if ((index++ % GCancellationCheckpointIntervalBytes) == 0)
        {
            RunCheckpoint(checkpoint);
        }
        if (!insideTag)
        {
            if (value == '<')
            {
                insideTag = true;
                ++shape.Nodes;
            }
            continue;
        }

        if (quote != '\0')
        {
            if (value == quote)
            {
                quote = '\0';
            }
            continue;
        }

        if (value == '\'' || value == '"')
        {
            quote = value;
        }
        else if (value == '=')
        {
            ++shape.Attributes;
        }
        else if (value == '>')
        {
            insideTag = false;
        }
    }
}

[[nodiscard]] SCatalogXmlAnalysis AnalyzeCatalogXml(
    const std::string_view text,
    const std::function<void()>& checkpoint = {})
{
    SCatalogXmlAnalysis analysis;
    analysis.DescriptionRange = FindDescriptionRange(text, checkpoint);
    if (!analysis.DescriptionRange.has_value())
    {
        AddXmlFragmentShape(analysis.Shape, text, checkpoint);
        return analysis;
    }

    const auto& range = *analysis.DescriptionRange;
    AddXmlFragmentShape(
        analysis.Shape,
        text.substr(
            range.RootOpenTag.Start,
            range.RootOpenTag.End - range.RootOpenTag.Start + 1),
        checkpoint);
    AddXmlFragmentShape(
        analysis.Shape,
        text.substr(
            range.DescriptionOpenTag.Start,
            range.DescriptionCloseTag.End - range.DescriptionOpenTag.Start + 1),
        checkpoint);

    const auto rootNameBytes = static_cast<std::uint64_t>(
        range.RootOpenTag.NameEnd - range.RootOpenTag.NameStart);
    analysis.Shape.Bytes += rootNameBytes + 3;
    ++analysis.Shape.Nodes;
    return analysis;
}

void ValidateMetadataXmlShape(
    const SMetadataXmlShape& shape,
    const std::string_view sourceLabel)
{
    const std::string label = sourceLabel.empty() ? std::string{"<memory>"} : std::string{sourceLabel};
    if (shape.Bytes > GMaxFb2MetadataXmlBytes)
    {
        throw std::runtime_error(
            "FB2 metadata XML from " + label + " requires " + std::to_string(shape.Bytes)
            + " bytes, exceeding the " + std::to_string(GMaxFb2MetadataXmlBytes)
            + " byte metadata limit.");
    }
    if (shape.Nodes > GMaxFb2MetadataXmlNodes)
    {
        throw std::runtime_error(
            "FB2 metadata XML from " + label + " has " + std::to_string(shape.Nodes)
            + " structural nodes, exceeding the " + std::to_string(GMaxFb2MetadataXmlNodes)
            + " node limit.");
    }
    if (shape.Attributes > GMaxFb2MetadataXmlAttributes)
    {
        throw std::runtime_error(
            "FB2 metadata XML from " + label + " has " + std::to_string(shape.Attributes)
            + " attributes, exceeding the " + std::to_string(GMaxFb2MetadataXmlAttributes)
            + " attribute limit.");
    }
}

[[nodiscard]] std::optional<SDescriptionRange> FindDescriptionRange(
    const std::string_view text,
    const std::function<void()>& checkpoint)
{
    const auto rootOpenTag = FindNextTagRange(
        text,
        0,
        false,
        "FictionBook",
        checkpoint);
    if (!rootOpenTag.has_value())
    {
        return std::nullopt;
    }

    const auto descOpenTag = FindNextTagRange(
        text,
        rootOpenTag->End + 1,
        false,
        "description",
        checkpoint);
    if (!descOpenTag.has_value())
    {
        return std::nullopt;
    }

    const auto descCloseTag = FindNextTagRange(
        text,
        descOpenTag->End + 1,
        true,
        "description",
        checkpoint);
    if (!descCloseTag.has_value())
    {
        return std::nullopt;
    }

    return SDescriptionRange{
        .RootOpenTag = *rootOpenTag,
        .DescriptionOpenTag = *descOpenTag,
        .DescriptionCloseTag = *descCloseTag
    };
}

[[nodiscard]] std::string BuildDescriptionXml(
    const std::string_view text,
    const SDescriptionRange& range)
{
    const std::string_view rootQualifiedName = text.substr(
        range.RootOpenTag.NameStart,
        range.RootOpenTag.NameEnd - range.RootOpenTag.NameStart);
    const std::string_view rootOpenTag = text.substr(
        range.RootOpenTag.Start,
        range.RootOpenTag.End - range.RootOpenTag.Start + 1);
    const std::string_view description = text.substr(
        range.DescriptionOpenTag.Start,
        range.DescriptionCloseTag.End - range.DescriptionOpenTag.Start + 1);
    std::string result;
    result.reserve(rootOpenTag.size() + description.size() + rootQualifiedName.size() + 3);
    result.append(rootOpenTag);
    result.append(description);
    result.append("</");
    result.append(rootQualifiedName);
    result.push_back('>');
    return result;
}

[[nodiscard]] SCatalogXmlDocument ParseCatalogXml(
    const std::string_view text,
    const std::string_view sourceLabel,
    const std::stop_token stopToken)
{
    const auto checkpoint = [stopToken]() {
        ThrowIfCancelled(stopToken);
    };
    const SCatalogXmlAnalysis analysis = AnalyzeCatalogXml(text, checkpoint);
    ValidateMetadataXmlShape(analysis.Shape, sourceLabel);
    if (analysis.DescriptionRange.has_value())
    {
        const auto& descriptionRange = *analysis.DescriptionRange;
        const std::string descXml = BuildDescriptionXml(text, descriptionRange);

        pugi::xml_document document;
        const pugi::xml_parse_result parseResult = document.load_buffer(descXml.data(), descXml.size());
        if (!parseResult || !document.first_child())
        {
            throw std::runtime_error(
                "Failed to parse FB2 metadata XML from " + std::string{sourceLabel}
                + ": " + parseResult.description()
                + " [size_bytes=" + std::to_string(text.size())
                + ", xml_preview=\"" + CompactPreview(descXml) + "\"]");
        }

        return SCatalogXmlDocument{
            .Document = std::move(document),
            .BinarySearchOffset = descriptionRange.DescriptionCloseTag.End + 1
        };
    }

    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(text.data(), text.size());

    if (result && document.first_child())
    {
        return SCatalogXmlDocument{
            .Document = std::move(document),
            .BinarySearchOffset = 0
        };
    }

    throw std::runtime_error(
        "Failed to parse FB2 XML from " + std::string{sourceLabel}
        + ": " + result.description()
        + " [size_bytes=" + std::to_string(text.size())
        + ", xml_preview=\"" + CompactPreview(text) + "\"]");
}

[[nodiscard]] std::string Trim(const std::string_view value)
{
    std::size_t start = 0;
    std::size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start])))
    {
        ++start;
    }

    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }

    return std::string{value.substr(start, end - start)};
}

[[nodiscard]] bool MatchesLocalName(const pugi::xml_node& node, const std::string_view localName)
{
    return MatchesLocalName(node.name(), localName);
}

[[nodiscard]] bool MatchesLocalName(const pugi::xml_attribute& attribute, const std::string_view localName)
{
    return MatchesLocalName(attribute.name(), localName);
}

[[nodiscard]] pugi::xml_node FindFirstChildByLocalName(const pugi::xml_node& parent, const std::string_view localName)
{
    for (const pugi::xml_node childNode : parent.children())
    {
        if (MatchesLocalName(childNode, localName))
        {
            return childNode;
        }
    }

    return {};
}

[[nodiscard]] std::string JoinAuthorName(const pugi::xml_node& authorNode)
{
    std::vector<std::string> parts;

    for (const char* partName : {"first-name", "middle-name", "last-name", "nickname"})
    {
        const std::string value = Trim(FindFirstChildByLocalName(authorNode, partName).text().as_string());

        if (!value.empty())
        {
            parts.push_back(value);
        }
    }

    std::string author;

    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (index > 0)
        {
            author.push_back(' ');
        }

        author.append(parts[index]);
    }

    return author;
}

[[nodiscard]] std::size_t CountAuthorNodes(const pugi::xml_node& parent)
{
    std::size_t count = 0;

    for (const pugi::xml_node childNode : parent.children())
    {
        if (MatchesLocalName(childNode, "author"))
        {
            ++count;
        }
    }

    return count;
}

[[nodiscard]] std::string BuildNodePreview(const pugi::xml_node& node)
{
    if (!node)
    {
        return "<missing>";
    }

    CCompactPreviewXmlWriter writer;
    try
    {
        node.print(writer, "", pugi::format_raw);
    }
    catch (const CPreviewLimitReached&)
    {
        return writer.Finish();
    }
    return writer.Finish();
}

[[nodiscard]] std::optional<std::string> TryReadTextChild(const pugi::xml_node& parent, const char* childName)
{
    const pugi::xml_node node = FindFirstChildByLocalName(parent, childName);
    std::string value = Trim(node.text().as_string());

    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] std::optional<std::string> TryReadNodeText(const pugi::xml_node& node)
{
    std::string value = Trim(node.text().as_string());

    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

template <typename TValue>
[[nodiscard]] std::optional<TValue> TryParseNumber(const std::string_view value) noexcept
{
    TValue parsedValue = {};
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto [parseEnd, errorCode] = std::from_chars(begin, end, parsedValue);

    if (errorCode != std::errc{} || parseEnd != end)
    {
        return std::nullopt;
    }

    return parsedValue;
}

[[nodiscard]] std::string ReadAnnotationText(const pugi::xml_node& annotationNode)
{
    std::string description;

    for (const pugi::xml_node childNode : annotationNode.children())
    {
        if (childNode.type() != pugi::node_element)
        {
            continue;
        }

        const std::string text = Trim(childNode.text().as_string());

        if (text.empty())
        {
            continue;
        }

        if (!description.empty())
        {
            description.push_back('\n');
        }

        description.append(text);
    }

    return description;
}

[[nodiscard]] std::string NormalizeExtension(std::string extension)
{
    if (!extension.empty() && extension.front() == '.')
    {
        extension.erase(extension.begin());
    }

    return Foundation::ToLowerAscii(std::move(extension));
}

[[nodiscard]] std::string ExtensionFromBinaryId(const std::string_view binaryId)
{
    const std::size_t dot = binaryId.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= binaryId.size())
    {
        return {};
    }

    return NormalizeExtension(std::string{binaryId.substr(dot + 1)});
}

[[nodiscard]] std::string MimeTypeToExtension(const std::string_view mimeType)
{
    if (mimeType == "image/jpeg" || mimeType == "image/jpg")
    {
        return "jpg";
    }

    if (mimeType == "image/png")
    {
        return "png";
    }

    if (mimeType == "image/gif")
    {
        return "gif";
    }

    return {};
}

[[nodiscard]] bool IsSelfClosingTag(const std::string_view openTag)
{
    std::size_t current = openTag.size();
    while (current > 0)
    {
        --current;
        const char value = openTag[current];
        if (std::isspace(static_cast<unsigned char>(value)) || value == '>')
        {
            continue;
        }

        return value == '/';
    }

    return false;
}

[[nodiscard]] std::optional<std::string> TryReadOpenTagAttribute(
    const std::string_view openTag,
    const std::string_view localName)
{
    const std::size_t tagEnd = openTag.rfind('>');
    if (tagEnd == std::string_view::npos || openTag.empty() || openTag.front() != '<')
    {
        return std::nullopt;
    }
    if (openTag.size() > GMaxXmlOpenTagBytes)
    {
        throw std::runtime_error("FB2 XML open tag exceeds the 65536 byte structural limit.");
    }

    std::size_t nameEnd = 1;
    while (nameEnd < tagEnd && IsXmlNameChar(openTag[nameEnd]))
    {
        ++nameEnd;
    }

    std::string probeTag = "<probe";
    if (nameEnd < tagEnd)
    {
        std::string_view attributes = openTag.substr(nameEnd, tagEnd - nameEnd);
        while (!attributes.empty() && std::isspace(static_cast<unsigned char>(attributes.front())))
        {
            attributes.remove_prefix(1);
        }

        while (!attributes.empty() && std::isspace(static_cast<unsigned char>(attributes.back())))
        {
            attributes.remove_suffix(1);
        }

        if (!attributes.empty() && attributes.back() == '/')
        {
            attributes.remove_suffix(1);
        }

        probeTag.push_back(' ');
        probeTag.append(attributes);
    }
    probeTag.append("/>");

    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(probeTag.data(), probeTag.size());
    if (!result)
    {
        return std::nullopt;
    }

    const pugi::xml_node probeNode = document.first_child();
    for (const pugi::xml_attribute attribute : probeNode.attributes())
    {
        if (MatchesLocalName(attribute, localName))
        {
            return std::string{attribute.as_string()};
        }
    }

    return std::nullopt;
}

struct SBinaryPayload
{
    std::string Id;
    std::string ContentType;
    std::string_view Content;
};

[[nodiscard]] std::optional<SBinaryPayload> FindBinaryPayloadById(
    const std::string_view text,
    const std::size_t startOffset,
    const std::string_view id,
    const std::stop_token stopToken)
{
    const auto checkpoint = [stopToken]() {
        ThrowIfCancelled(stopToken);
    };
    std::size_t currentOffset = startOffset;

    while (const auto binaryOpenTag = FindNextTagRange(
        text,
        currentOffset,
        false,
        "binary",
        checkpoint))
    {
        ThrowIfCancelled(stopToken);
        const std::string_view openTag = text.substr(
            binaryOpenTag->Start,
            binaryOpenTag->End - binaryOpenTag->Start + 1);
        const std::optional<std::string> binaryId = TryReadOpenTagAttribute(openTag, "id");
        const std::optional<std::string> contentType = TryReadOpenTagAttribute(openTag, "content-type");

        if (IsSelfClosingTag(openTag))
        {
            if (binaryId.has_value() && *binaryId == id)
            {
                return SBinaryPayload{
                    .Id = *binaryId,
                    .ContentType = contentType.value_or(std::string{}),
                    .Content = {}
                };
            }

            currentOffset = binaryOpenTag->End + 1;
            continue;
        }

        const auto binaryCloseTag = FindNextTagRange(
            text,
            binaryOpenTag->End + 1,
            true,
            "binary",
            checkpoint);
        if (!binaryCloseTag.has_value())
        {
            break;
        }

        if (binaryId.has_value() && *binaryId == id)
        {
            return SBinaryPayload{
                .Id = *binaryId,
                .ContentType = contentType.value_or(std::string{}),
                .Content = text.substr(
                    binaryOpenTag->End + 1,
                    binaryCloseTag->Start - binaryOpenTag->End - 1)
            };
        }

        currentOffset = binaryCloseTag->End + 1;
    }

    return std::nullopt;
}

[[nodiscard]] std::vector<std::byte> DecodeBase64(
    const std::string_view encodedValue,
    const std::stop_token stopToken)
{
    const auto DecodeCharacter = [](const char value) -> std::uint8_t {
        if (value >= 'A' && value <= 'Z')
        {
            return static_cast<std::uint8_t>(value - 'A');
        }

        if (value >= 'a' && value <= 'z')
        {
            return static_cast<std::uint8_t>(value - 'a' + 26);
        }

        if (value >= '0' && value <= '9')
        {
            return static_cast<std::uint8_t>(value - '0' + 52);
        }

        if (value == '+')
        {
            return 62;
        }

        if (value == '/')
        {
            return 63;
        }

        throw std::runtime_error("Invalid base64 character in FB2 binary data.");
    };

    ThrowIfCancelled(stopToken);
    std::string sanitized;
    sanitized.reserve(encodedValue.size());

    std::size_t inputIndex = 0;
    for (const char value : encodedValue)
    {
        if ((inputIndex++ & 0xFFFFu) == 0)
        {
            ThrowIfCancelled(stopToken);
        }
        if (!std::isspace(static_cast<unsigned char>(value)))
        {
            sanitized.push_back(value);
        }
    }

    if (sanitized.empty())
    {
        return {};
    }

    if (sanitized.size() % 4 != 0)
    {
        throw std::runtime_error("Invalid base64 length in FB2 binary data.");
    }

    std::size_t paddingCount = 0;
    if (sanitized.back() == '=')
    {
        paddingCount = 1;
        if (sanitized[sanitized.size() - 2] == '=')
        {
            paddingCount = 2;
        }
    }

    const std::size_t firstPadding = sanitized.find('=');
    if (firstPadding != std::string::npos && firstPadding != sanitized.size() - paddingCount)
    {
        throw std::runtime_error("Invalid base64 padding in FB2 binary data.");
    }

    std::vector<std::byte> bytes;
    bytes.reserve((sanitized.size() / 4) * 3 - paddingCount);

    for (std::size_t index = 0; index < sanitized.size(); index += 4)
    {
        if ((index & 0xFFFFu) == 0)
        {
            ThrowIfCancelled(stopToken);
        }
        const char a = sanitized[index];
        const char b = sanitized[index + 1];
        const char c = sanitized[index + 2];
        const char d = sanitized[index + 3];

        const std::uint8_t aValue = DecodeCharacter(a);
        const std::uint8_t bValue = DecodeCharacter(b);
        const std::uint8_t cValue = c == '=' ? 0 : DecodeCharacter(c);
        const std::uint8_t dValue = d == '=' ? 0 : DecodeCharacter(d);

        if ((c == '=' && (bValue & 0x0Fu) != 0)
            || (d == '=' && c != '=' && (cValue & 0x03u) != 0))
        {
            throw std::runtime_error("Invalid base64 padding bits in FB2 binary data.");
        }

        const std::uint32_t block =
            (static_cast<std::uint32_t>(aValue) << 18) |
            (static_cast<std::uint32_t>(bValue) << 12) |
            (static_cast<std::uint32_t>(cValue) << 6) |
            static_cast<std::uint32_t>(dValue);

        bytes.push_back(static_cast<std::byte>((block >> 16) & 0xFF));

        if (c != '=')
        {
            bytes.push_back(static_cast<std::byte>((block >> 8) & 0xFF));
        }

        if (d != '=')
        {
            bytes.push_back(static_cast<std::byte>(block & 0xFF));
        }
    }

    return bytes;
}

[[nodiscard]] std::optional<std::string> TryGetCoverBinaryId(const pugi::xml_node& titleInfoNode)
{
    const pugi::xml_node imageNode = FindFirstChildByLocalName(FindFirstChildByLocalName(titleInfoNode, "coverpage"), "image");

    if (!imageNode)
    {
        return std::nullopt;
    }

    for (const pugi::xml_attribute attribute : imageNode.attributes())
    {
        if (MatchesLocalName(attribute, "href"))
        {
            std::string value = attribute.as_string();

            if (!value.empty() && value.front() == '#')
            {
                value.erase(value.begin());
            }

            if (!value.empty())
            {
                return value;
            }
        }
    }

    return std::nullopt;
}

[[nodiscard]] InpxWebReader::Domain::SParsedBook ParseDecodedFb2Text(
    const std::string& text,
    const std::string_view sourceLabel,
    const InpxWebReader::Domain::SBookParseOptions& options,
    const std::size_t parserWarningCount)
{
    ThrowIfCancelled(options.StopToken);
    const SCatalogXmlDocument catalogXml = ParseCatalogXml(text, sourceLabel, options.StopToken);
    ThrowIfCancelled(options.StopToken);
    const pugi::xml_document& document = catalogXml.Document;
    const pugi::xml_node rootNode = FindFirstChildByLocalName(document, "FictionBook");
    const pugi::xml_node descriptionNode = FindFirstChildByLocalName(rootNode, "description");
    const pugi::xml_node titleInfoNode = FindFirstChildByLocalName(descriptionNode, "title-info");

    if (!rootNode || !titleInfoNode)
    {
        throw std::runtime_error("FB2 document is missing required description/title-info nodes.");
    }

    InpxWebReader::Domain::SParsedBook parsedBook;
    parsedBook.ParserWarningCount = parserWarningCount;
    const std::optional<std::string> title = TryReadNodeText(FindFirstChildByLocalName(titleInfoNode, "book-title"));

    if (title.has_value())
    {
        parsedBook.Metadata.TitleUtf8 = *title;
    }
    else if (options.MissingTitleFallbackUtf8.has_value() && !options.MissingTitleFallbackUtf8->empty())
    {
        parsedBook.Metadata.TitleUtf8 = *options.MissingTitleFallbackUtf8;
        ++parsedBook.ParserWarningCount;
        InpxWebReader::Logging::WarnIfInitialized(
            "FB2 missing required <book-title> node in '{}'; using caller-provided fallback title.",
            sourceLabel);
    }
    else
    {
        throw std::runtime_error("Missing required FB2 node: book-title");
    }

    if (const std::optional<std::string> lang = TryReadTextChild(titleInfoNode, "lang"))
    {
        parsedBook.Metadata.Language = *lang;
    }
    else
    {
        if (!options.MissingLanguageMayBeRecoveredByCaller)
        {
            ++parsedBook.ParserWarningCount;
            InpxWebReader::Logging::WarnIfInitialized(
                "FB2 missing required <lang> node in '{}'; language will be empty.",
                sourceLabel);
        }
        parsedBook.Metadata.Language = "";
    }

    for (const pugi::xml_node authorNode : titleInfoNode.children())
    {
        ThrowIfCancelled(options.StopToken);
        if (!MatchesLocalName(authorNode, "author"))
        {
            continue;
        }

        const std::string author = JoinAuthorName(authorNode);

        if (!author.empty())
        {
            parsedBook.Metadata.AuthorsUtf8.push_back(author);
        }
    }

    if (parsedBook.Metadata.AuthorsUtf8.empty())
    {
        ++parsedBook.ParserWarningCount;
        InpxWebReader::Logging::WarnIfInitialized(
            "FB2 title-info has no non-empty author nodes; using 'Аноним' fallback."
            " [title_info_author_nodes={}, title_info_preview=\"{}\", document_info_preview=\"{}\", file=\"{}\"]",
            CountAuthorNodes(titleInfoNode),
            BuildNodePreview(titleInfoNode),
            BuildNodePreview(FindFirstChildByLocalName(descriptionNode, "document-info")),
            sourceLabel);
        parsedBook.Metadata.AuthorsUtf8.push_back("Аноним");
    }

    for (const pugi::xml_node childNode : titleInfoNode.children())
    {
        ThrowIfCancelled(options.StopToken);
        if (MatchesLocalName(childNode, "genre"))
        {
            const std::optional<std::string> rawCode = TryReadNodeText(childNode);
            if (!rawCode.has_value())
                continue;

            std::string_view remaining = std::string_view{rawCode->data(), rawCode->size()};
            while (!remaining.empty())
            {
                const std::size_t commaPos = remaining.find(',');
                std::string_view token = (commaPos != std::string_view::npos)
                    ? remaining.substr(0, commaPos)
                    : remaining;

                const auto first = token.find_first_not_of(" \t\r\n\v\f");
                if (first != std::string_view::npos)
                {
                    const auto last = token.find_last_not_of(" \t\r\n\v\f");
                    token = token.substr(first, last - first + 1);
                }
                else
                {
                    token = {};
                }

                if (!token.empty())
                {
                    const SGenreResolution resolvedGenre = CFb2GenreMapper::ResolveGenre(token);
                    if (!resolvedGenre.IsKnown)
                    {
                        ++parsedBook.ParserWarningCount;
                        InpxWebReader::Logging::WarnIfInitialized(
                            "FB2 unknown genre code '{}' (no mapping) in file: {}",
                            std::string{token},
                            sourceLabel);
                    }
                    parsedBook.Metadata.GenresUtf8.push_back(std::string{resolvedGenre.DisplayName});
                }

                if (commaPos == std::string_view::npos)
                    break;
                remaining = remaining.substr(commaPos + 1);
            }
        }
    }

    if (const std::optional<std::string> sequenceName = TryReadTextChild(titleInfoNode.child("sequence"), "name"))
    {
        parsedBook.Metadata.SeriesUtf8 = sequenceName;
    }
    else if (const char* sequenceValue = FindFirstChildByLocalName(titleInfoNode, "sequence").attribute("name").as_string(); sequenceValue != nullptr && sequenceValue[0] != '\0')
    {
        parsedBook.Metadata.SeriesUtf8 = std::string{sequenceValue};
    }

    if (const char* sequenceNumber = FindFirstChildByLocalName(titleInfoNode, "sequence").attribute("number").as_string(); sequenceNumber != nullptr && sequenceNumber[0] != '\0')
    {
        const auto parsed = TryParseNumber<double>(sequenceNumber);
        if (parsed.has_value())
        {
            parsedBook.Metadata.SeriesIndex = parsed;
        }
        else
        {
            ++parsedBook.ParserWarningCount;
            InpxWebReader::Logging::WarnIfInitialized(
                "FB2 non-numeric sequence number '{}' skipped in file: {}",
                sequenceNumber,
                sourceLabel);
        }
    }

    if (const pugi::xml_node annotationNode = FindFirstChildByLocalName(titleInfoNode, "annotation"))
    {
        const std::string description = ReadAnnotationText(annotationNode);

        if (!description.empty())
        {
            parsedBook.Metadata.DescriptionUtf8 = description;
        }
    }

    if (const pugi::xml_node publishInfoNode = FindFirstChildByLocalName(descriptionNode, "publish-info"))
    {
        if (const std::optional<std::string> isbn = TryReadTextChild(publishInfoNode, "isbn"))
        {
            parsedBook.Metadata.Isbn = isbn;
        }

        if (const std::optional<std::string> publisher = TryReadTextChild(publishInfoNode, "publisher"))
        {
            parsedBook.Metadata.PublisherUtf8 = publisher;
        }

        if (const std::optional<std::string> year = TryReadTextChild(publishInfoNode, "year"))
        {
            const auto parsed = TryParseNumber<int>(*year);
            if (parsed.has_value())
            {
                parsedBook.Metadata.Year = parsed;
            }
            else
            {
                ++parsedBook.ParserWarningCount;
                InpxWebReader::Logging::WarnIfInitialized(
                    "FB2 non-integer publish year '{}' skipped in file: {}",
                    *year,
                    sourceLabel);
            }
        }
    }

    if (const pugi::xml_node documentInfoNode = FindFirstChildByLocalName(descriptionNode, "document-info"))
    {
        if (const std::optional<std::string> identifier = TryReadTextChild(documentInfoNode, "id"))
        {
            parsedBook.Metadata.Identifier = identifier;
        }
    }

    if (!options.ExtractCover)
    {
        return parsedBook;
    }

    if (const std::optional<std::string> coverBinaryId = TryGetCoverBinaryId(titleInfoNode))
    {
        const std::optional<SBinaryPayload> binaryPayload = FindBinaryPayloadById(
            text,
            catalogXml.BinarySearchOffset,
            *coverBinaryId,
            options.StopToken);

        if (binaryPayload.has_value())
        {
            try
            {
                parsedBook.CoverBytes = DecodeBase64(binaryPayload->Content, options.StopToken);
            }
            catch (const std::exception& ex)
            {
                ThrowIfCancelled(options.StopToken);
                InpxWebReader::Logging::WarnIfInitialized(
                    "cover: base64 decode failed binaryId='{}' reason='{}' source='{}'",
                    *coverBinaryId,
                    ex.what(),
                    sourceLabel);
                parsedBook.CoverDiagnosticMessage = "base64-decode-failed";
            }

            if (!parsedBook.CoverBytes.empty())
            {
                std::string extension = MimeTypeToExtension(binaryPayload->ContentType);

                if (extension.empty())
                {
                    extension = ExtensionFromBinaryId(binaryPayload->Id);
                }

                if (!extension.empty())
                {
                    parsedBook.CoverExtension = extension;
                }
            }
        }
        else
        {
            parsedBook.CoverDiagnosticMessage = "referenced-binary-not-found";
            InpxWebReader::Logging::WarnIfInitialized(
                "cover: referenced binary not found binaryId='{}' source='{}'",
                *coverBinaryId,
                sourceLabel);
        }

        if (binaryPayload.has_value() && parsedBook.CoverBytes.empty() && !parsedBook.CoverDiagnosticMessage.has_value())
        {
            parsedBook.CoverDiagnosticMessage = "decoded-cover-empty";
        }

        if (binaryPayload.has_value() && !parsedBook.CoverBytes.empty() && !parsedBook.CoverExtension.has_value())
        {
            parsedBook.CoverDiagnosticMessage = "decoded-cover-missing-extension";
        }
    }
    else if (const pugi::xml_node coverPageNode = FindFirstChildByLocalName(titleInfoNode, "coverpage"))
    {
        if (FindFirstChildByLocalName(coverPageNode, "image"))
        {
            parsedBook.CoverDiagnosticMessage = "cover-reference-missing-binary-id";
            InpxWebReader::Logging::WarnIfInitialized(
                "cover: image node present but binary id missing source='{}'",
                sourceLabel);
        }
    }

    return parsedBook;
}

} // namespace

InpxWebReader::Domain::SParsedBook CFb2Parser::ParseBytes(
    std::string rawBytes,
    const std::string_view logicalSourceLabel,
    const InpxWebReader::Domain::SBookParseOptions& options) const
{
    const std::string sourceLabel = logicalSourceLabel.empty()
        ? std::string{"<memory>"}
        : std::string{logicalSourceLabel};
    if (!InpxWebReader::Foundation::IsBookPayloadSizeAllowed(rawBytes.size()))
    {
        throw std::runtime_error(
            "FB2 payload is too large: " + sourceLabel + " (" + std::to_string(rawBytes.size())
            + " bytes exceeds " + std::to_string(InpxWebReader::Foundation::GMaxBookPayloadBytes)
            + " byte limit)");
    }

    ThrowIfCancelled(options.StopToken);
    std::size_t parserWarningCount = 0;
    const std::string text = DecodeFb2Text(
        std::move(rawBytes),
        sourceLabel,
        parserWarningCount,
        options.StopToken);
    ThrowIfCancelled(options.StopToken);
    return ParseDecodedFb2Text(text, sourceLabel, options, parserWarningCount);
}
} // namespace InpxWebReader::Fb2Parsing
