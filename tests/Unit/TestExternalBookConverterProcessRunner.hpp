#pragma once

#include <functional>
#include <optional>
#include <stop_token>

#include "Converter/ExternalConverterProcessRunner.hpp"

class CFakeProcessRunner final : public InpxWebReader::ConverterRuntime::IExternalConverterProcessRunner
{
public:
    [[nodiscard]] InpxWebReader::ConverterRuntime::SExternalConverterProcessResult Run(
        const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request,
        InpxWebReader::Domain::IProgressSink& progressSink,
        std::stop_token stopToken) const override
    {
        ++Calls;
        LastRequest = request;
        LastProgressSinkCancellationState = progressSink.IsCancellationRequested();
        LastStopRequested = stopToken.stop_requested();

        if (OnRun)
        {
            OnRun(request);
        }

        return Result;
    }

    mutable int Calls = 0;
    mutable std::optional<InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest> LastRequest = std::nullopt;
    mutable bool LastProgressSinkCancellationState = false;
    mutable bool LastStopRequested = false;
    InpxWebReader::ConverterRuntime::SExternalConverterProcessResult Result;
    std::function<void(const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request)> OnRun;
};
