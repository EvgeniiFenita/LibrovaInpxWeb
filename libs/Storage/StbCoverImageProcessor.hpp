#pragma once

#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::CoverProcessingStb {

class CStbCoverImageProcessor final : public InpxWebReader::Domain::ICoverImageProcessor
{
public:
    [[nodiscard]] InpxWebReader::Domain::SCoverProcessingResult ProcessForCache(
        const InpxWebReader::Domain::SCoverProcessingRequest& request) const override;
};

} // namespace InpxWebReader::CoverProcessingStb
