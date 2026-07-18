find_package(Stb REQUIRED)

add_library(InpxWebReaderStorage STATIC
    InpxArchivePathResolver.cpp
    InpxArchivePathResolver.hpp
    PathSafety.cpp
    PathSafety.hpp
    InpxCacheLayout.cpp
    InpxCacheLayout.hpp
    RuntimeWorkspaceLayout.hpp
    StbCoverImageProcessor.cpp
    StbCoverImageProcessor.hpp
)

target_include_directories(InpxWebReaderStorage
    PUBLIC
        ${CMAKE_SOURCE_DIR}/libs
    PRIVATE
        ${Stb_INCLUDE_DIR}
)

target_link_libraries(InpxWebReaderStorage
    PUBLIC
        InpxWebReaderDomain
        InpxWebReaderFoundation
)

target_compile_features(InpxWebReaderStorage PUBLIC cxx_std_20)
