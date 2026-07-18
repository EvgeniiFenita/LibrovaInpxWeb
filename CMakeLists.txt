cmake_minimum_required(VERSION 3.26)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "InpxWebReader supports Linux builds only.")
endif()

# CMAKE_TOOLCHAIN_FILE is consumed by CMake before project() for toolchain
# loading; that internal read is not tracked as a CMakeLists.txt usage, so
# CMake emits a false-positive "not used" warning. Reference it here to
# suppress the warning without changing any behaviour.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" INPX_WEB_READER_PRODUCT_VERSION LIMIT_COUNT 1)
string(STRIP "${INPX_WEB_READER_PRODUCT_VERSION}" INPX_WEB_READER_PRODUCT_VERSION)

project(InpxWebReader VERSION ${INPX_WEB_READER_PRODUCT_VERSION} LANGUAGES CXX)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" INPX_WEB_READER_SYSTEM_PROCESSOR)
if(NOT INPX_WEB_READER_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "InpxWebReader supports only x86_64 Linux builds; detected ${CMAKE_SYSTEM_PROCESSOR}.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INPX_WEB_READER_BUILD_WEB_ASSETS "Build web UI static assets." OFF)
option(INPX_WEB_READER_BUILD_FUZZERS "Build Clang libFuzzer targets." OFF)

include(CTest)

add_subdirectory(libs/Domain)
add_subdirectory(libs/Foundation)
add_subdirectory(libs/Converter)
add_subdirectory(libs/Storage)
add_subdirectory(libs/Database)
add_subdirectory(libs/Parsing)
add_subdirectory(libs/ScanSupport)
add_subdirectory(libs/Inpx)
add_subdirectory(libs/App)
add_subdirectory(libs/Server)

if(INPX_WEB_READER_BUILD_WEB_ASSETS)
    add_subdirectory(web/inpx-web-reader)
endif()

add_subdirectory(apps/InpxWebReader)
add_subdirectory(tests/Unit)

if(INPX_WEB_READER_BUILD_FUZZERS)
    add_subdirectory(tests/Fuzz)
endif()
