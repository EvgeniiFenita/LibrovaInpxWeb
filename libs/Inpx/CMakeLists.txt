find_package(libzip CONFIG REQUIRED)

add_library(InpxWebReaderInpx STATIC
    InpxSourceConfiguration.cpp
    InpxParser.cpp
)

target_include_directories(InpxWebReaderInpx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(InpxWebReaderInpx
    PUBLIC
        InpxWebReaderFoundation
        libzip::zip
)

target_compile_features(InpxWebReaderInpx PUBLIC cxx_std_20)
