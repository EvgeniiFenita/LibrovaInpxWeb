#include "Server/BookListCursor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Domain/MetadataNormalization.hpp"
#include "Foundation/Sha256Fingerprint.hpp"

namespace InpxWebReader::Server {
namespace {

constexpr std::size_t GMaxEncodedCursorBytes = 8192;
constexpr std::string_view GBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] std::string EncodeBase64Url(const std::string_view bytes)
{
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    std::uint32_t buffer = 0;
    int bitCount = 0;
    for (const unsigned char byte : bytes)
    {
        buffer = (buffer << 8U) | byte;
        bitCount += 8;
        while (bitCount >= 6)
        {
            bitCount -= 6;
            result.push_back(GBase64UrlAlphabet[(buffer >> bitCount) & 0x3FU]);
        }
    }
    if (bitCount > 0)
    {
        result.push_back(GBase64UrlAlphabet[(buffer << (6 - bitCount)) & 0x3FU]);
    }
    return result;
}

[[nodiscard]] int DecodeBase64UrlCharacter(const char value) noexcept
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    if (value == '-')
    {
        return 62;
    }
    if (value == '_')
    {
        return 63;
    }
    return -1;
}

[[nodiscard]] std::string DecodeBase64Url(const std::string_view encoded)
{
    if (encoded.empty() || encoded.size() > GMaxEncodedCursorBytes || encoded.size() % 4 == 1)
    {
        throw std::invalid_argument("Catalog cursor encoding is invalid.");
    }

    std::string result;
    result.reserve((encoded.size() * 3) / 4);
    std::uint32_t buffer = 0;
    int bitCount = 0;
    for (const char character : encoded)
    {
        const int value = DecodeBase64UrlCharacter(character);
        if (value < 0)
        {
            throw std::invalid_argument("Catalog cursor encoding is invalid.");
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
        bitCount += 6;
        if (bitCount >= 8)
        {
            bitCount -= 8;
            result.push_back(static_cast<char>((buffer >> bitCount) & 0xFFU));
        }
    }
    if (bitCount > 0 && (buffer & ((1U << bitCount) - 1U)) != 0)
    {
        throw std::invalid_argument("Catalog cursor encoding is invalid.");
    }
    return result;
}

[[nodiscard]] std::vector<std::string> CanonicalizeValues(
    std::vector<std::string> values,
    const bool normalize)
{
    if (normalize)
    {
        for (auto& value : values)
        {
            value = Domain::NormalizeText(value);
        }
    }
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

[[nodiscard]] std::string BuildQueryFingerprint(const Application::SBookListRequest& request)
{
    auto fields = request.SearchFields;
    if (!fields.HasAnyField())
    {
        fields = {};
    }
    const auto sort = request.SortBy.value_or(Domain::EBookSort::Title);
    const auto direction = request.SortDirection.value_or(Domain::ESortDirection::Ascending);
    const nlohmann::json canonical{
        {"text", Domain::NormalizeText(request.TextUtf8)},
        {"fields", {
            {"title", fields.Title},
            {"authors", fields.Authors},
            {"description", fields.Description}
        }},
        {"languages", CanonicalizeValues(request.Languages, false)},
        {"genres", CanonicalizeValues(request.GenresUtf8, true)},
        {"sort", static_cast<int>(sort)},
        {"direction", static_cast<int>(direction)}
    };
    return Foundation::CSha256Fingerprint::ComputeBytes(canonical.dump());
}

[[nodiscard]] bool IsSessionId(const std::string_view value) noexcept
{
    return value.size() == 32
        && std::ranges::all_of(value, [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        });
}

} // namespace

std::string CBookListCursorCodec::Encode(
    const Domain::SSearchCursor& cursor,
    const Application::SBookListRequest& request)
{
    if (!cursor.IsValid() || !IsSessionId(cursor.SessionIdUtf8))
    {
        throw std::invalid_argument("Catalog cursor state is invalid.");
    }
    return EncodeBase64Url(nlohmann::json{
        {"version", 1},
        {"session", cursor.SessionIdUtf8},
        {"source", cursor.SourceFingerprintUtf8},
        {"snapshot", cursor.CatalogSnapshotIdUtf8},
        {"position", cursor.Position},
        {"query", BuildQueryFingerprint(request)}
    }.dump());
}

Domain::SSearchCursor CBookListCursorCodec::Decode(
    const std::string_view encodedCursor,
    const Application::SBookListRequest& request)
{
    try
    {
        const auto payload = nlohmann::json::parse(DecodeBase64Url(encodedCursor));
        if (!payload.is_object()
            || payload.size() != 6
            || payload.at("version").get<int>() != 1
            || payload.at("query").get<std::string>() != BuildQueryFingerprint(request))
        {
            throw std::invalid_argument("Catalog cursor does not match this query.");
        }

        Domain::SSearchCursor cursor{
            .SessionIdUtf8 = payload.at("session").get<std::string>(),
            .SourceFingerprintUtf8 = payload.at("source").get<std::string>(),
            .CatalogSnapshotIdUtf8 = payload.at("snapshot").get<std::string>(),
            .Position = payload.at("position").get<std::size_t>()
        };
        if (!cursor.IsValid()
            || !IsSessionId(cursor.SessionIdUtf8)
            || cursor.SourceFingerprintUtf8.size() > 256
            || cursor.CatalogSnapshotIdUtf8.size() > 256)
        {
            throw std::invalid_argument("Catalog cursor state is invalid.");
        }
        return cursor;
    }
    catch (const std::invalid_argument&)
    {
        throw;
    }
    catch (...)
    {
        throw std::invalid_argument("Catalog cursor is invalid.");
    }
}

} // namespace InpxWebReader::Server
