find_package(spdlog CONFIG REQUIRED)
find_package(Iconv REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(InpxWebReaderFoundation STATIC
    BookPayloadLimits.hpp
    FilenamePolicy.cpp
    FilenamePolicy.hpp
    FileSystemUtils.cpp
    FileSystemUtils.hpp
    Logging.cpp
    Logging.hpp
    Sha256Fingerprint.cpp
    Sha256Fingerprint.hpp
    StringUtils.cpp
    StringUtils.hpp
    UnicodeConversion.cpp
    UnicodeConversion.hpp
    Version.cpp
    Version.hpp
    ZipArchivePaths.cpp
    ZipArchivePaths.hpp
)

target_include_directories(InpxWebReaderFoundation PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_compile_features(InpxWebReaderFoundation PUBLIC cxx_std_20)

target_compile_definitions(InpxWebReaderFoundation PUBLIC
    INPX_WEB_READER_PRODUCT_VERSION="${INPX_WEB_READER_PRODUCT_VERSION}"
)

target_link_libraries(InpxWebReaderFoundation
    PUBLIC
        spdlog::spdlog_header_only
    PRIVATE
        Iconv::Iconv
        OpenSSL::Crypto
)
