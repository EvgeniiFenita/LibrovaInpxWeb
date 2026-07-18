add_library(InpxWebReaderDomain STATIC
    BookFormat.cpp
    DomainError.cpp
    InpxBookAvailability.hpp
    LanguageNormalization.cpp
    LanguageNormalization.hpp
    MetadataNormalization.cpp
)

target_include_directories(InpxWebReaderDomain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(InpxWebReaderDomain
    PUBLIC
        InpxWebReaderFoundation
)

target_compile_features(InpxWebReaderDomain PUBLIC cxx_std_20)
