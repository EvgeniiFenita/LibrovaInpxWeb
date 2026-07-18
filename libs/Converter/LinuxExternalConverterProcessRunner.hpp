#pragma once

#include "Converter/ExternalConverterProcessRunner.hpp"

namespace InpxWebReader::ConverterRuntime {

class CLinuxExternalConverterProcessRunner final : public IExternalConverterProcessRunner
{
public:
    [[nodiscard]] SExternalConverterProcessResult Run(
        const SExternalConverterProcessRequest& request,
        InpxWebReader::Domain::IProgressSink& progressSink,
        std::stop_token stopToken) const override;
};

} // namespace InpxWebReader::ConverterRuntime
