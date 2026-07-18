#include "Foundation/Version.hpp"

namespace InpxWebReader::Core {

std::string_view CVersion::GetValue() noexcept
{
    return INPX_WEB_READER_PRODUCT_VERSION;
}

} // namespace InpxWebReader::Core
