find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(INPX_WEB_READER_WEB_STAMP "${CMAKE_CURRENT_BINARY_DIR}/.inpx-web-reader-assets.stamp")

file(GLOB_RECURSE INPX_WEB_READER_WEB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/public/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/playwright.config.ts"
    "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/vite.config.ts"
    "${CMAKE_SOURCE_DIR}/scripts/RunWebUi.py"
    "${CMAKE_SOURCE_DIR}/scripts/_common.py"
)

add_custom_command(
    OUTPUT "${INPX_WEB_READER_WEB_STAMP}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/RunWebUi.py" "--artifacts-root" "${CMAKE_CURRENT_BINARY_DIR}" "build"
    COMMAND "${CMAKE_COMMAND}" -E touch "${INPX_WEB_READER_WEB_STAMP}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS ${INPX_WEB_READER_WEB_SOURCES}
    COMMENT "Building InpxWebReader web UI assets"
    VERBATIM
)

add_custom_target(InpxWebReaderWebAssets ALL
    DEPENDS "${INPX_WEB_READER_WEB_STAMP}"
)
