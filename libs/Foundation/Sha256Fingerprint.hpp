#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace InpxWebReader::Foundation {

class CSha256FingerprintBuilder final
{
public:
    CSha256FingerprintBuilder();
    ~CSha256FingerprintBuilder();

    CSha256FingerprintBuilder(const CSha256FingerprintBuilder&) = delete;
    CSha256FingerprintBuilder& operator=(const CSha256FingerprintBuilder&) = delete;
    CSha256FingerprintBuilder(CSha256FingerprintBuilder&&) noexcept;
    CSha256FingerprintBuilder& operator=(CSha256FingerprintBuilder&&) noexcept;

    void Update(std::string_view bytes);
    [[nodiscard]] std::string Finalize();

private:
    class CImpl;
    std::unique_ptr<CImpl> m_impl;
};

class CSha256Fingerprint final
{
public:
    [[nodiscard]] static std::string ComputeFile(
        const std::filesystem::path& path,
        const std::function<void()>& checkpoint = {});
    [[nodiscard]] static std::string ComputeBytes(std::string_view bytes);
    [[nodiscard]] static std::string Normalize(std::string_view fingerprint);
};

} // namespace InpxWebReader::Foundation
