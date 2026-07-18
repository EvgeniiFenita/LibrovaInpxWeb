find_path(BSHOSHANY_THREAD_POOL_INCLUDE_DIRS "BS_thread_pool.hpp")

add_library(InpxWebReaderScanSupport STATIC
    ScanConcurrency.hpp
    CoverCacheProcessing.cpp
    CoverCacheProcessing.hpp
    ScanPerfScopes.hpp
    ScanPerfTracker.cpp
)

target_include_directories(InpxWebReaderScanSupport PUBLIC "${CMAKE_SOURCE_DIR}/libs")

target_link_libraries(InpxWebReaderScanSupport
    PUBLIC
        InpxWebReaderDomain
        InpxWebReaderFoundation
    PRIVATE
        InpxWebReaderParsing
)

target_include_directories(InpxWebReaderScanSupport PRIVATE ${BSHOSHANY_THREAD_POOL_INCLUDE_DIRS})
