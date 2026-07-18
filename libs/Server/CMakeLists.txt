find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# cpp-httplib enables glibc getaddrinfo_a by default. Its helper thread crashes
# inside the ThreadSanitizer v3 allocator before an HTTP request can be made.
# The server has no outbound DNS use, and its test clients connect to numeric
# loopback addresses, so keep the portable blocking resolver instead.
get_target_property(httplib_compile_definitions httplib::httplib INTERFACE_COMPILE_DEFINITIONS)
if(httplib_compile_definitions)
    list(
        FILTER httplib_compile_definitions
        EXCLUDE REGEX "CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO"
    )
    set_property(
        TARGET httplib::httplib
        PROPERTY INTERFACE_COMPILE_DEFINITIONS ${httplib_compile_definitions}
    )
endif()

add_library(InpxWebReaderServer STATIC
    AuthMiddleware.cpp
    AuthMiddleware.hpp
    BackendExecutor.cpp
    BackendExecutor.hpp
    BookListCursor.cpp
    BookListCursor.hpp
    HttpError.cpp
    HttpError.hpp
    HttpServer.cpp
    HttpServer.hpp
    JsonDtoMapper.cpp
    JsonDtoMapper.hpp
    ServerApplicationHost.cpp
    ServerApplicationHost.hpp
    ServerCommandLine.cpp
    ServerCommandLine.hpp
    ServerConfig.cpp
    ServerConfig.hpp
    ServerScanCoordinator.cpp
    ServerScanCoordinator.hpp
)

target_include_directories(InpxWebReaderServer PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(InpxWebReaderServer
    PUBLIC
        InpxWebReaderApp
        InpxWebReaderFoundation
        httplib::httplib
        nlohmann_json::nlohmann_json
    PRIVATE
        InpxWebReaderConverter
        InpxWebReaderInpx
        InpxWebReaderStorage
)

target_compile_features(InpxWebReaderServer PUBLIC cxx_std_20)
