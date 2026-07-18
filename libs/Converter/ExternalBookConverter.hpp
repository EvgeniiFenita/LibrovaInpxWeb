#pragma once

#include <chrono>

#include "Converter/ConverterCommandBuilder.hpp"
#include "Converter/ExternalConverterProcessRunner.hpp"
#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::ConverterRuntime {

struct SExternalConverterSettings
{
    InpxWebReader::ConverterCommand::SConverterCommandProfile CommandProfile;
    std::filesystem::path WorkingDirectory;
    std::chrono::milliseconds PollInterval = std::chrono::milliseconds{100};
    std::chrono::milliseconds Timeout = std::chrono::minutes{5};
    bool RevalidateBuiltInFbcBeforeRun = false;
    SExternalConverterProcessHooks ProcessHooks;
    const IExternalConverterProcessRunner* ProcessRunner = nullptr;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return CommandProfile.IsValid() && PollInterval.count() > 0 && Timeout.count() > 0;
    }
};

class CExternalBookConverter final : public InpxWebReader::Domain::IBookConverter
{
public:
    explicit CExternalBookConverter(SExternalConverterSettings settings);

    [[nodiscard]] bool CanConvert(
        InpxWebReader::Domain::EBookFormat sourceFormat,
        InpxWebReader::Domain::EBookFormat destinationFormat) const override;

    [[nodiscard]] InpxWebReader::Domain::SConversionResult Convert(
        const InpxWebReader::Domain::SConversionRequest& request,
        InpxWebReader::Domain::IProgressSink& progressSink,
        std::stop_token stopToken) const override;

private:
    SExternalConverterSettings m_settings;
    const IExternalConverterProcessRunner* m_processRunner = nullptr;
};

} // namespace InpxWebReader::ConverterRuntime
