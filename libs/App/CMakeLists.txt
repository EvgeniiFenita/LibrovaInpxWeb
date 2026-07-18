find_path(BSHOSHANY_THREAD_POOL_INCLUDE_DIRS "BS_thread_pool.hpp")

add_library(InpxWebReaderApp STATIC
    CInpxCatalogApplication.cpp
    CInpxCatalogApplication.hpp
    InpxArchiveAccess.cpp
    InpxArchiveAccess.hpp
    InpxScanJobService.cpp
    InpxScanJobService.hpp
    IInpxCatalogApplication.hpp
    InpxCacheBootstrap.cpp
    InpxCacheBootstrap.hpp
    InpxCatalogFacade.cpp
    InpxCatalogFacade.hpp
    BookDownloadFacade.cpp
    BookDownloadFacade.hpp
    FileReplacement.cpp
    FileReplacement.hpp
)

target_include_directories(InpxWebReaderApp
    PUBLIC
        ${CMAKE_SOURCE_DIR}/libs
    PRIVATE
        ${BSHOSHANY_THREAD_POOL_INCLUDE_DIRS}
)

target_link_libraries(InpxWebReaderApp
    PUBLIC
        InpxWebReaderDomain
        InpxWebReaderScanSupport
        InpxWebReaderInpx
        InpxWebReaderConverter
    PRIVATE
        InpxWebReaderDatabase
        InpxWebReaderParsing
        InpxWebReaderFoundation
        InpxWebReaderStorage
)

target_compile_features(InpxWebReaderApp PUBLIC cxx_std_20)
