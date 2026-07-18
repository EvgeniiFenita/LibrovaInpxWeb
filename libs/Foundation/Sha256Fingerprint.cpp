#include "Foundation/Sha256Fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>

#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Foundation {
namespace {

constexpr std::size_t GReadBufferSizeBytes = std::size_t{1024} * 1024;
constexpr std::string_view GSha256Prefix = "sha256:";
constexpr std::size_t GSha256HexDigitCount = 64;

using TDigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

[[nodiscard]] bool IsAsciiHexDigit(const char value) noexcept
{
    return (value >= '0' && value <= '9')
        || (value >= 'a' && value <= 'f')
        || (value >= 'A' && value <= 'F');
}

[[nodiscard]] TDigestContext CreateContext()
{
    TDigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    {
        throw std::runtime_error("Could not initialize SHA-256.");
    }
    return context;
}

void Update(EVP_MD_CTX* const context, const void* const bytes, const std::size_t size)
{
    if (size > 0 && EVP_DigestUpdate(context, bytes, size) != 1)
    {
        throw std::runtime_error("Could not update SHA-256.");
    }
}

[[nodiscard]] std::string Finalize(EVP_MD_CTX* const context)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context, digest.data(), &digestSize) != 1
        || digestSize != static_cast<unsigned int>(EVP_MD_get_size(EVP_sha256())))
    {
        throw std::runtime_error("Could not finalize SHA-256.");
    }

    constexpr char hexDigits[] = "0123456789abcdef";
    std::string result{GSha256Prefix};
    result.reserve(result.size() + (static_cast<std::size_t>(digestSize) * 2));
    for (unsigned int index = 0; index < digestSize; ++index)
    {
        result.push_back(hexDigits[digest[index] >> 4U]);
        result.push_back(hexDigits[digest[index] & 0x0FU]);
    }
    return result;
}

} // namespace

class CSha256FingerprintBuilder::CImpl final
{
public:
    TDigestContext Context = CreateContext();
    bool Finalized = false;
};

CSha256FingerprintBuilder::CSha256FingerprintBuilder()
    : m_impl(std::make_unique<CImpl>())
{
}

CSha256FingerprintBuilder::~CSha256FingerprintBuilder() = default;

CSha256FingerprintBuilder::CSha256FingerprintBuilder(CSha256FingerprintBuilder&&) noexcept = default;

CSha256FingerprintBuilder& CSha256FingerprintBuilder::operator=(
    CSha256FingerprintBuilder&&) noexcept = default;

void CSha256FingerprintBuilder::Update(const std::string_view bytes)
{
    if (m_impl == nullptr || m_impl->Finalized)
    {
        throw std::logic_error("Cannot update a finalized SHA-256 fingerprint.");
    }
    Foundation::Update(m_impl->Context.get(), bytes.data(), bytes.size());
}

std::string CSha256FingerprintBuilder::Finalize()
{
    if (m_impl == nullptr || m_impl->Finalized)
    {
        throw std::logic_error("Cannot finalize a SHA-256 fingerprint more than once.");
    }
    m_impl->Finalized = true;
    return Foundation::Finalize(m_impl->Context.get());
}

std::string CSha256Fingerprint::ComputeFile(
    const std::filesystem::path& path,
    const std::function<void()>& checkpoint)
{
    const auto runCheckpoint = [&]() {
        if (checkpoint)
        {
            checkpoint();
        }
    };
    runCheckpoint();
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Could not open file for SHA-256: '" + Unicode::PathToUtf8(path) + "'.");
    }

    auto context = CreateContext();
    std::array<char, GReadBufferSizeBytes> buffer{};
    while (input)
    {
        runCheckpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        Update(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof())
    {
        throw std::runtime_error("Could not read file for SHA-256: '" + Unicode::PathToUtf8(path) + "'.");
    }
    return Finalize(context.get());
}

std::string CSha256Fingerprint::ComputeBytes(const std::string_view bytes)
{
    auto context = CreateContext();
    Update(context.get(), bytes.data(), bytes.size());
    return Finalize(context.get());
}

std::string CSha256Fingerprint::Normalize(const std::string_view fingerprint)
{
    const std::string_view value = fingerprint.starts_with(GSha256Prefix)
        ? fingerprint.substr(GSha256Prefix.size())
        : fingerprint;
    if (value.size() != GSha256HexDigitCount
        || !std::ranges::all_of(value, IsAsciiHexDigit))
    {
        throw std::invalid_argument("Expected a SHA-256 fingerprint.");
    }
    return std::string{GSha256Prefix} + ToLowerAscii(std::string{value});
}

} // namespace InpxWebReader::Foundation
