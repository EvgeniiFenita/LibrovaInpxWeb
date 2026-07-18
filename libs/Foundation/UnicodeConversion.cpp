#include "Foundation/UnicodeConversion.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <iconv.h>

namespace {

constexpr std::size_t GCancellationCheckpointIntervalBytes = 64ull * 1024ull;

void RunCheckpoint(const std::function<void()>& checkpoint)
{
    if (checkpoint)
    {
        checkpoint();
    }
}

class CTextConversionError final : public std::runtime_error
{
public:
    explicit CTextConversionError(const std::string_view message)
        : std::runtime_error(std::string{message})
    {
    }
};

[[nodiscard]] iconv_t InvalidIconvDescriptor() noexcept
{
    // iconv_open returns (iconv_t)-1 on failure.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<iconv_t>(-1);
}

[[nodiscard]] bool IsInvalidIconvDescriptor(const iconv_t descriptor) noexcept
{
    return descriptor == InvalidIconvDescriptor();
}

class CIconvDescriptor final
{
public:
    CIconvDescriptor(
        const std::string_view toEncoding,
        const std::string_view fromEncoding,
        const std::string_view errorContext)
    {
        const std::string to{toEncoding};
        const std::string from{fromEncoding};
        m_descriptor = iconv_open(to.c_str(), from.c_str());
        if (IsInvalidIconvDescriptor(m_descriptor))
        {
            throw CTextConversionError(errorContext);
        }
    }

    ~CIconvDescriptor()
    {
        if (!IsInvalidIconvDescriptor(m_descriptor))
        {
            iconv_close(m_descriptor);
        }
    }

    CIconvDescriptor(const CIconvDescriptor&) = delete;
    CIconvDescriptor& operator=(const CIconvDescriptor&) = delete;
    CIconvDescriptor(CIconvDescriptor&&) = delete;
    CIconvDescriptor& operator=(CIconvDescriptor&&) = delete;

    [[nodiscard]] iconv_t Get() const noexcept
    {
        return m_descriptor;
    }

private:
    iconv_t m_descriptor = InvalidIconvDescriptor();
};

void GrowOutputBuffer(std::string& output, char*& outputCursor, std::size_t& outputBytesLeft)
{
    const std::size_t bytesWritten = output.size() - outputBytesLeft;
    if (output.size() > output.max_size() - GCancellationCheckpointIntervalBytes)
    {
        throw std::length_error("Unicode conversion output exceeds its implementation limit.");
    }
    output.resize(output.size() + GCancellationCheckpointIntervalBytes);
    outputCursor = output.data() + bytesWritten;
    outputBytesLeft = output.size() - bytesWritten;
}

[[nodiscard]] std::string ConvertText(
    const std::string_view value,
    const std::string_view fromEncoding,
    const std::string_view toEncoding,
    const std::string_view errorContext,
    const std::function<void()>& checkpoint)
{
    RunCheckpoint(checkpoint);
    if (value.empty())
    {
        return {};
    }

    CIconvDescriptor converter(toEncoding, fromEncoding, errorContext);

    if (value.size() > std::string{}.max_size() / 4)
    {
        throw std::length_error("Unicode conversion input exceeds its implementation limit.");
    }
    const std::size_t maximumExpectedOutputSize = std::max<std::size_t>(32, value.size() * 4);
    std::string output;
    output.reserve(maximumExpectedOutputSize);
    output.resize((std::min)(maximumExpectedOutputSize, GCancellationCheckpointIntervalBytes));

    auto* inputCursor = const_cast<char*>(value.data());
    std::size_t inputBytesLeft = value.size();
    char* outputCursor = output.data();
    std::size_t outputBytesLeft = output.size();

    while (inputBytesLeft > 0)
    {
        RunCheckpoint(checkpoint);
        if (outputBytesLeft == 0)
        {
            GrowOutputBuffer(output, outputCursor, outputBytesLeft);
        }
        const std::size_t offeredOutputBytes = (std::min)(
            outputBytesLeft,
            GCancellationCheckpointIntervalBytes);
        char* callOutputCursor = outputCursor;
        std::size_t callOutputBytesLeft = offeredOutputBytes;
        errno = 0;
        const std::size_t result = iconv(
            converter.Get(),
            &inputCursor,
            &inputBytesLeft,
            &callOutputCursor,
            &callOutputBytesLeft);
        const std::size_t producedBytes = offeredOutputBytes - callOutputBytesLeft;
        outputCursor = callOutputCursor;
        outputBytesLeft -= producedBytes;
        if (result != static_cast<std::size_t>(-1))
        {
            continue;
        }

        if (errno == E2BIG)
        {
            if (outputBytesLeft == 0)
            {
                GrowOutputBuffer(output, outputCursor, outputBytesLeft);
            }
            continue;
        }

        throw CTextConversionError(errorContext);
    }

    for (;;)
    {
        RunCheckpoint(checkpoint);
        if (outputBytesLeft == 0)
        {
            GrowOutputBuffer(output, outputCursor, outputBytesLeft);
        }
        const std::size_t offeredOutputBytes = (std::min)(
            outputBytesLeft,
            GCancellationCheckpointIntervalBytes);
        char* callOutputCursor = outputCursor;
        std::size_t callOutputBytesLeft = offeredOutputBytes;
        errno = 0;
        const std::size_t result = iconv(
            converter.Get(),
            nullptr,
            nullptr,
            &callOutputCursor,
            &callOutputBytesLeft);
        const std::size_t producedBytes = offeredOutputBytes - callOutputBytesLeft;
        outputCursor = callOutputCursor;
        outputBytesLeft -= producedBytes;
        if (result != static_cast<std::size_t>(-1))
        {
            break;
        }

        if (errno == E2BIG)
        {
            if (outputBytesLeft == 0)
            {
                GrowOutputBuffer(output, outputCursor, outputBytesLeft);
            }
            continue;
        }

        throw CTextConversionError(errorContext);
    }

    output.resize(output.size() - outputBytesLeft);
    return output;
}

[[nodiscard]] std::vector<std::string> CodePageToIconvEncodings(const unsigned int codePage)
{
    switch (codePage)
    {
    case 866:
        return {"CP866", "IBM866"};
    case 1251:
        return {"WINDOWS-1251", "CP1251"};
    case 65001:
        return {"UTF-8"};
    case 1200:
        return {"UTF-16LE"};
    case 1201:
        return {"UTF-16BE"};
    default:
        return {"CP" + std::to_string(codePage)};
    }
}

[[nodiscard]] bool TryDecodeUtf8CodePoint(
    const std::string_view value,
    const std::size_t index,
    char32_t& codePoint,
    std::size_t& sequenceLength) noexcept
{
    const unsigned char firstByte = static_cast<unsigned char>(value[index]);

    if (firstByte < 0x80)
    {
        codePoint = firstByte;
        sequenceLength = 1;
        return true;
    }

    if ((firstByte & 0xE0) == 0xC0)
    {
        sequenceLength = 2;
        if (index + sequenceLength > value.size())
        {
            return false;
        }

        const unsigned char secondByte = static_cast<unsigned char>(value[index + 1]);
        if (firstByte < 0xC2 || firstByte > 0xDF || secondByte < 0x80 || secondByte > 0xBF)
        {
            return false;
        }

        codePoint = (static_cast<char32_t>(firstByte & 0x1F) << 6)
            | static_cast<char32_t>(secondByte & 0x3F);
        return true;
    }

    if ((firstByte & 0xF0) == 0xE0)
    {
        sequenceLength = 3;
        if (index + sequenceLength > value.size())
        {
            return false;
        }

        const unsigned char secondByte = static_cast<unsigned char>(value[index + 1]);
        const unsigned char thirdByte = static_cast<unsigned char>(value[index + 2]);
        const bool isValidSequence =
            (firstByte == 0xE0 && secondByte >= 0xA0 && secondByte <= 0xBF && thirdByte >= 0x80 && thirdByte <= 0xBF)
            || (firstByte >= 0xE1 && firstByte <= 0xEC
                && secondByte >= 0x80 && secondByte <= 0xBF
                && thirdByte >= 0x80 && thirdByte <= 0xBF)
            || (firstByte == 0xED && secondByte >= 0x80 && secondByte <= 0x9F
                && thirdByte >= 0x80 && thirdByte <= 0xBF)
            || (firstByte >= 0xEE && firstByte <= 0xEF
                && secondByte >= 0x80 && secondByte <= 0xBF
                && thirdByte >= 0x80 && thirdByte <= 0xBF);
        if (!isValidSequence)
        {
            return false;
        }

        codePoint = (static_cast<char32_t>(firstByte & 0x0F) << 12)
            | (static_cast<char32_t>(secondByte & 0x3F) << 6)
            | static_cast<char32_t>(thirdByte & 0x3F);
        return true;
    }

    if ((firstByte & 0xF8) == 0xF0)
    {
        sequenceLength = 4;
        if (index + sequenceLength > value.size())
        {
            return false;
        }

        const unsigned char secondByte = static_cast<unsigned char>(value[index + 1]);
        const unsigned char thirdByte = static_cast<unsigned char>(value[index + 2]);
        const unsigned char fourthByte = static_cast<unsigned char>(value[index + 3]);
        const bool isValidSequence =
            (firstByte == 0xF0 && secondByte >= 0x90 && secondByte <= 0xBF
                && thirdByte >= 0x80 && thirdByte <= 0xBF
                && fourthByte >= 0x80 && fourthByte <= 0xBF)
            || (firstByte >= 0xF1 && firstByte <= 0xF3
                && secondByte >= 0x80 && secondByte <= 0xBF
                && thirdByte >= 0x80 && thirdByte <= 0xBF
                && fourthByte >= 0x80 && fourthByte <= 0xBF)
            || (firstByte == 0xF4 && secondByte >= 0x80 && secondByte <= 0x8F
                && thirdByte >= 0x80 && thirdByte <= 0xBF
                && fourthByte >= 0x80 && fourthByte <= 0xBF);
        if (!isValidSequence)
        {
            return false;
        }

        codePoint = (static_cast<char32_t>(firstByte & 0x07) << 18)
            | (static_cast<char32_t>(secondByte & 0x3F) << 12)
            | (static_cast<char32_t>(thirdByte & 0x3F) << 6)
            | static_cast<char32_t>(fourthByte & 0x3F);
        return true;
    }

    return false;
}

[[nodiscard]] std::optional<std::size_t> ExpectedUtf8SequenceLength(const unsigned char firstByte) noexcept
{
    if (firstByte < 0x80)
    {
        return 1;
    }

    if (firstByte >= 0xC2 && firstByte <= 0xDF)
    {
        return 2;
    }

    if (firstByte >= 0xE0 && firstByte <= 0xEF)
    {
        return 3;
    }

    if (firstByte >= 0xF0 && firstByte <= 0xF4)
    {
        return 4;
    }

    return std::nullopt;
}

[[nodiscard]] bool IsUtf8ContinuationByte(const unsigned char value) noexcept
{
    return value >= 0x80 && value <= 0xBF;
}

[[nodiscard]] bool CanBeSecondUtf8Byte(const unsigned char firstByte, const unsigned char secondByte) noexcept
{
    if (firstByte == 0xE0)
    {
        return secondByte >= 0xA0 && secondByte <= 0xBF;
    }

    if (firstByte == 0xED)
    {
        return secondByte >= 0x80 && secondByte <= 0x9F;
    }

    if (firstByte == 0xF0)
    {
        return secondByte >= 0x90 && secondByte <= 0xBF;
    }

    if (firstByte == 0xF4)
    {
        return secondByte >= 0x80 && secondByte <= 0x8F;
    }

    return IsUtf8ContinuationByte(secondByte);
}

[[nodiscard]] bool IsTrailingIncompleteUtf8Sequence(
    const std::string_view value,
    const std::size_t startIndex,
    const std::size_t expectedLength) noexcept
{
    const std::size_t remainingBytes = value.size() - startIndex;
    if (remainingBytes == 0 || remainingBytes >= expectedLength)
    {
        return false;
    }

    if (remainingBytes == 1)
    {
        return true;
    }

    const auto firstByte = static_cast<unsigned char>(value[startIndex]);
    const auto secondByte = static_cast<unsigned char>(value[startIndex + 1]);
    if (!CanBeSecondUtf8Byte(firstByte, secondByte))
    {
        return false;
    }

    for (std::size_t offset = 2; offset < remainingBytes; ++offset)
    {
        if (!IsUtf8ContinuationByte(static_cast<unsigned char>(value[startIndex + offset])))
        {
            return false;
        }
    }

    return true;
}

void AppendUtf8CodePoint(const char32_t codePoint, std::string& result)
{
    if (codePoint <= 0x7F)
    {
        result.push_back(static_cast<char>(codePoint));
        return;
    }

    if (codePoint <= 0x7FF)
    {
        result.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        return;
    }

    if (codePoint <= 0xFFFF)
    {
        result.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        return;
    }

    result.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
}

[[nodiscard]] bool IsUnicodeWhitespace(const char32_t codePoint) noexcept
{
    switch (codePoint)
    {
    case U'\u0009':
    case U'\u000A':
    case U'\u000B':
    case U'\u000C':
    case U'\u000D':
    case U'\u0020':
    case U'\u0085':
    case U'\u00A0':
    case U'\u1680':
    case U'\u2028':
    case U'\u2029':
    case U'\u202F':
    case U'\u205F':
    case U'\u3000':
        return true;
    default:
        return codePoint >= U'\u2000' && codePoint <= U'\u200A';
    }
}

[[nodiscard]] long long ScoreLegacyCyrillicDecode(
    const std::string_view value,
    const std::function<void()>& checkpoint)
{
    long long score = 0;
    std::size_t cyrillicWordLength = 0;
    std::size_t cyrillicVowelCount = 0;
    const auto finishCyrillicWord = [&]() {
        if (cyrillicWordLength >= 3)
        {
            if (cyrillicVowelCount == 0)
            {
                score -= 5;
            }
            else if (cyrillicWordLength >= 5
                && cyrillicVowelCount * 5 >= cyrillicWordLength)
            {
                score += 2;
            }
        }
        cyrillicWordLength = 0;
        cyrillicVowelCount = 0;
    };
    std::size_t nextCheckpoint = 0;
    for (std::size_t index = 0; index < value.size();)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }
        char32_t codePoint = 0;
        std::size_t sequenceLength = 0;
        if (!TryDecodeUtf8CodePoint(value, index, codePoint, sequenceLength))
        {
            return (std::numeric_limits<long long>::min)();
        }

        if ((codePoint >= U'А' && codePoint <= U'я')
            || codePoint == U'Ё'
            || codePoint == U'ё')
        {
            score += 4;
            ++cyrillicWordLength;
            switch (codePoint)
            {
            case U'А': case U'а': case U'Е': case U'е': case U'Ё': case U'ё':
            case U'И': case U'и': case U'О': case U'о': case U'У': case U'у':
            case U'Ы': case U'ы': case U'Э': case U'э': case U'Ю': case U'ю':
            case U'Я': case U'я':
                ++cyrillicVowelCount;
                break;
            default:
                break;
            }
        }
        else if (codePoint >= U'Ѐ' && codePoint <= U'ԯ')
        {
            finishCyrillicWord();
            score += 1;
        }
        else if (codePoint >= U'─' && codePoint <= U'▟')
        {
            finishCyrillicWord();
            score -= 9;
        }
        else if (codePoint == U'�'
            || (codePoint >= U'\u0080' && codePoint <= U'\u009F'))
        {
            finishCyrillicWord();
            score -= 6;
        }
        else
        {
            finishCyrillicWord();
        }

        index += sequenceLength;
    }
    finishCyrillicWord();
    return score;
}

[[nodiscard]] char32_t CaseFoldCodePoint(const char32_t codePoint) noexcept
{
    if (codePoint <= 0x7F)
    {
        return static_cast<char32_t>(std::tolower(static_cast<unsigned char>(codePoint)));
    }

    if (codePoint >= U'\u00C0' && codePoint <= U'\u00D6')
    {
        return codePoint + 0x20;
    }

    if (codePoint >= U'\u00D8' && codePoint <= U'\u00DE')
    {
        return codePoint + 0x20;
    }

    if ((codePoint >= U'\u0100' && codePoint <= U'\u012E' && (codePoint % 2) == 0)
        || (codePoint >= U'\u0132' && codePoint <= U'\u0136' && (codePoint % 2) == 0)
        || (codePoint >= U'\u014A' && codePoint <= U'\u0176' && (codePoint % 2) == 0)
        || (codePoint >= U'\u01DE' && codePoint <= U'\u01EE' && (codePoint % 2) == 0)
        || (codePoint >= U'\u01F8' && codePoint <= U'\u021E' && (codePoint % 2) == 0)
        || (codePoint >= U'\u0222' && codePoint <= U'\u0232' && (codePoint % 2) == 0)
        || (codePoint >= U'\u0460' && codePoint <= U'\u04FE' && (codePoint % 2) == 0)
        || (codePoint >= U'\u0500' && codePoint <= U'\u052E' && (codePoint % 2) == 0))
    {
        return codePoint + 1;
    }

    if ((codePoint >= U'\u0139' && codePoint <= U'\u0148' && (codePoint % 2) == 1)
        || (codePoint >= U'\u0179' && codePoint <= U'\u017E' && (codePoint % 2) == 1))
    {
        return codePoint + 1;
    }

    switch (codePoint)
    {
    case U'\u0130':
        return U'i';
    case U'\u0178':
        return U'\u00FF';
    case U'\u01F1':
    case U'\u01F2':
        return U'\u01F3';
    case U'\u0386':
        return U'\u03AC';
    case U'\u0388':
        return U'\u03AD';
    case U'\u0389':
        return U'\u03AE';
    case U'\u038A':
        return U'\u03AF';
    case U'\u038C':
        return U'\u03CC';
    case U'\u038E':
        return U'\u03CD';
    case U'\u038F':
        return U'\u03CE';
    case U'\u03C2':
        return U'\u03C3';
    case U'\u0401':
    case U'\u0451':
        return U'\u0435';
    case U'\u1E9E':
        return U'\u00DF';
    default:
        break;
    }

    if (codePoint >= U'\u0391' && codePoint <= U'\u03A1')
    {
        return codePoint + 0x20;
    }

    if (codePoint >= U'\u03A3' && codePoint <= U'\u03AB')
    {
        return codePoint + 0x20;
    }

    if (codePoint >= U'\u0410' && codePoint <= U'\u042F')
    {
        return codePoint + 0x20;
    }

    if (codePoint >= U'\u0400' && codePoint <= U'\u040F')
    {
        switch (codePoint)
        {
        case U'\u0400':
            return U'\u0450';
        case U'\u0402':
            return U'\u0452';
        case U'\u0403':
            return U'\u0453';
        case U'\u0404':
            return U'\u0454';
        case U'\u0405':
            return U'\u0455';
        case U'\u0406':
            return U'\u0456';
        case U'\u0407':
            return U'\u0457';
        case U'\u0408':
            return U'\u0458';
        case U'\u0409':
            return U'\u0459';
        case U'\u040A':
            return U'\u045A';
        case U'\u040B':
            return U'\u045B';
        case U'\u040C':
            return U'\u045C';
        case U'\u040D':
            return U'\u045D';
        case U'\u040E':
            return U'\u045E';
        case U'\u040F':
            return U'\u045F';
        default:
            return codePoint;
        }
    }

    return codePoint;
}

void AppendCaseFoldedCodePoint(const char32_t codePoint, std::string& result)
{
    const char32_t foldedCodePoint = CaseFoldCodePoint(codePoint);
    if (foldedCodePoint == U'\u00DF')
    {
        result.append("ss");
        return;
    }

    AppendUtf8CodePoint(foldedCodePoint, result);
}

} // namespace

namespace InpxWebReader::Unicode {

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8Path = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
}

std::filesystem::path PathFromUtf8(const std::string_view path)
{
    const auto utf8Path = std::u8string{
        reinterpret_cast<const char8_t*>(path.data()),
        reinterpret_cast<const char8_t*>(path.data()) + path.size()
    };
    return std::filesystem::path{utf8Path};
}

std::string CodePageToUtf8(
    const std::string_view value,
    const unsigned int codePage,
    const std::string_view errorContext,
    const std::function<void()>& checkpoint)
{
    for (const std::string& encoding : CodePageToIconvEncodings(codePage))
    {
        try
        {
            return ConvertText(value, encoding, "UTF-8", errorContext, checkpoint);
        }
        catch (const CTextConversionError&)
        {
            RunCheckpoint(checkpoint);
            // Try the next iconv alias for the same code page.
            (void)0;
        }
    }

    throw CTextConversionError(errorContext);
}

std::string DecodeLegacyCyrillicToUtf8(
    const std::string_view value,
    const std::string_view errorContext,
    const std::function<void()>& checkpoint)
{
    std::optional<std::string> windows1251;
    std::optional<std::string> cp866;
    try
    {
        windows1251 = CodePageToUtf8(value, 1251, errorContext, checkpoint);
    }
    catch (const CTextConversionError&)
    {
        RunCheckpoint(checkpoint);
        windows1251 = Windows1251ToUtf8Lossy(value, checkpoint);
    }

    try
    {
        cp866 = CodePageToUtf8(value, 866, errorContext, checkpoint);
    }
    catch (const CTextConversionError&)
    {
        RunCheckpoint(checkpoint);
    }

    if (!windows1251.has_value() && !cp866.has_value())
    {
        throw std::runtime_error(std::string{errorContext});
    }
    if (!windows1251.has_value())
    {
        return std::move(*cp866);
    }
    if (!cp866.has_value())
    {
        return std::move(*windows1251);
    }

    const long long cp866Score = ScoreLegacyCyrillicDecode(*cp866, checkpoint);
    const long long windows1251Score = ScoreLegacyCyrillicDecode(*windows1251, checkpoint);
    if (cp866Score == windows1251Score && *cp866 != *windows1251)
    {
        throw std::runtime_error(
            std::string{errorContext}
            + ": legacy Cyrillic encoding is ambiguous between Windows-1251 and CP866.");
    }

    return cp866Score > windows1251Score ? std::move(*cp866) : std::move(*windows1251);
}

std::string Windows1251ToUtf8Lossy(
    const std::string_view value,
    const std::function<void()>& checkpoint)
{
    constexpr std::array<char32_t, 64> Windows1251HighControlBlock = {
        U'\u0402', U'\u0403', U'\u201A', U'\u0453', U'\u201E', U'\u2026', U'\u2020', U'\u2021',
        U'\u20AC', U'\u2030', U'\u0409', U'\u2039', U'\u040A', U'\u040C', U'\u040B', U'\u040F',
        U'\u0452', U'\u2018', U'\u2019', U'\u201C', U'\u201D', U'\u2022', U'\u2013', U'\u2014',
        U'\uFFFD', U'\u2122', U'\u0459', U'\u203A', U'\u045A', U'\u045C', U'\u045B', U'\u045F',
        U'\u00A0', U'\u040E', U'\u045E', U'\u0408', U'\u00A4', U'\u0490', U'\u00A6', U'\u00A7',
        U'\u0401', U'\u00A9', U'\u0404', U'\u00AB', U'\u00AC', U'\u00AD', U'\u00AE', U'\u0407',
        U'\u00B0', U'\u00B1', U'\u0406', U'\u0456', U'\u0491', U'\u00B5', U'\u00B6', U'\u00B7',
        U'\u0451', U'\u2116', U'\u0454', U'\u00BB', U'\u0458', U'\u0405', U'\u0455', U'\u0457'
    };

    std::string result;
    result.reserve(value.size() * 2);
    std::size_t index = 0;
    for (const unsigned char byte : value)
    {
        if ((index++ % GCancellationCheckpointIntervalBytes) == 0)
        {
            RunCheckpoint(checkpoint);
        }
        if (byte < 0x80)
        {
            AppendUtf8CodePoint(byte, result);
            continue;
        }

        if (byte >= 0xC0)
        {
            AppendUtf8CodePoint(U'\u0410' + static_cast<char32_t>(byte - 0xC0), result);
            continue;
        }

        AppendUtf8CodePoint(Windows1251HighControlBlock[byte - 0x80], result);
    }

    return result;
}

bool IsValidUtf8(const std::string_view value) noexcept
{
    for (std::size_t index = 0; index < value.size();)
    {
        char32_t codePoint = 0;
        std::size_t sequenceLength = 0;
        if (!TryDecodeUtf8CodePoint(value, index, codePoint, sequenceLength))
        {
            return false;
        }

        index += sequenceLength;
    }

    return true;
}

bool IsValidUtf8(
    const std::string_view value,
    const std::function<void()>& checkpoint)
{
    std::size_t nextCheckpoint = 0;
    for (std::size_t index = 0; index < value.size();)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }
        char32_t codePoint = 0;
        std::size_t sequenceLength = 0;
        if (!TryDecodeUtf8CodePoint(value, index, codePoint, sequenceLength))
        {
            return false;
        }

        index += sequenceLength;
    }
    RunCheckpoint(checkpoint);
    return true;
}

std::optional<std::string> TryTrimTrailingIncompleteUtf8(
    const std::string_view value,
    const std::function<void()>& checkpoint)
{
    std::size_t nextCheckpoint = 0;
    for (std::size_t index = 0; index < value.size();)
    {
        if (index >= nextCheckpoint)
        {
            RunCheckpoint(checkpoint);
            nextCheckpoint = index + GCancellationCheckpointIntervalBytes;
        }
        const auto firstByte = static_cast<unsigned char>(value[index]);
        const std::optional<std::size_t> expectedLength = ExpectedUtf8SequenceLength(firstByte);
        if (!expectedLength.has_value())
        {
            return std::nullopt;
        }

        if (IsTrailingIncompleteUtf8Sequence(value, index, *expectedLength))
        {
            return std::string{value.substr(0, index)};
        }

        char32_t codePoint = 0;
        std::size_t sequenceLength = 0;
        if (!TryDecodeUtf8CodePoint(value, index, codePoint, sequenceLength))
        {
            return std::nullopt;
        }

        index += sequenceLength;
    }

    return std::nullopt;
}

std::optional<std::string> TryNormalizeUtf8WhitespaceAndCaseFold(const std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());
    bool previousWasWhitespace = true;

    for (std::size_t index = 0; index < value.size();)
    {
        char32_t codePoint = 0;
        std::size_t sequenceLength = 0;
        if (!TryDecodeUtf8CodePoint(value, index, codePoint, sequenceLength))
        {
            return std::nullopt;
        }

        if (IsUnicodeWhitespace(codePoint))
        {
            if (!previousWasWhitespace)
            {
                normalized.push_back(' ');
                previousWasWhitespace = true;
            }

            index += sequenceLength;
            continue;
        }

        AppendCaseFoldedCodePoint(codePoint, normalized);
        previousWasWhitespace = false;
        index += sequenceLength;
    }

    if (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }

    return normalized;
}

std::string Utf16LeToUtf8(
    const void* const data,
    const std::size_t byteCount,
    const std::function<void()>& checkpoint)
{
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    if (byteCount >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFFu
        && static_cast<unsigned char>(bytes[1]) == 0xFEu)
    {
        offset = 2;
    }

    if ((byteCount - offset) % 2 != 0)
    {
        throw std::runtime_error("Failed to convert UTF-16 LE text to UTF-8: odd byte count.");
    }

    return ConvertText(
        std::string_view{bytes + offset, byteCount - offset},
        "UTF-16LE",
        "UTF-8",
        "Failed to convert UTF-16 LE text to UTF-8.",
        checkpoint);
}

std::string Utf16BeToUtf8(
    const void* const data,
    const std::size_t byteCount,
    const std::function<void()>& checkpoint)
{
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    if (byteCount >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFEu
        && static_cast<unsigned char>(bytes[1]) == 0xFFu)
    {
        offset = 2;
    }

    if ((byteCount - offset) % 2 != 0)
    {
        throw std::runtime_error("Failed to convert UTF-16 BE text to UTF-8: odd byte count.");
    }

    return ConvertText(
        std::string_view{bytes + offset, byteCount - offset},
        "UTF-16BE",
        "UTF-8",
        "Failed to convert UTF-16 BE text to UTF-8.",
        checkpoint);
}

} // namespace InpxWebReader::Unicode
