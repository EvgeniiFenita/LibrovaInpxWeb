#pragma once

#include <filesystem>

#include "App/IInpxCatalogApplication.hpp"

namespace InpxWebReader::Application {

class CInpxCacheBootstrap final
{
public:
    static void PrepareCacheRoot(
        const std::filesystem::path& cacheRoot,
        ECacheOpenMode cacheOpenMode);

    static void ValidateExistingCacheRoot(const std::filesystem::path& cacheRoot);
};

} // namespace InpxWebReader::Application
