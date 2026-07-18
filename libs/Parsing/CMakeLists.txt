find_package(libzip CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

add_library(InpxWebReaderParsing STATIC
    Fb2GenreMapper.cpp
    Fb2Parser.cpp
)

target_include_directories(InpxWebReaderParsing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(InpxWebReaderParsing
    PUBLIC
        InpxWebReaderDomain
    PRIVATE
        InpxWebReaderFoundation
        libzip::zip
        pugixml::pugixml
)

target_compile_features(InpxWebReaderParsing PUBLIC cxx_std_20)
