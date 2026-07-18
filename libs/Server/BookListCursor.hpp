#pragma once

#include <string>
#include <string_view>

#include "App/InpxCatalogFacade.hpp"
#include "Domain/SearchQuery.hpp"

namespace InpxWebReader::Server {

class CBookListCursorCodec final
{
public:
    [[nodiscard]] static std::string Encode(
        const Domain::SSearchCursor& cursor,
        const Application::SBookListRequest& request);
    [[nodiscard]] static Domain::SSearchCursor Decode(
        std::string_view encodedCursor,
        const Application::SBookListRequest& request);
};

} // namespace InpxWebReader::Server
