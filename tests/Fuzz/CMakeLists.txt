if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "InpxWebReader fuzz targets require Clang and libFuzzer.")
endif()

add_executable(InpxWebReaderFb2Fuzzer
    FuzzFb2Parser.cpp
)

add_executable(InpxWebReaderInpxArchiveFuzzer
    FuzzInpxArchive.cpp
)

target_link_libraries(InpxWebReaderFb2Fuzzer
    PRIVATE
        InpxWebReaderParsing
)

target_link_libraries(InpxWebReaderInpxArchiveFuzzer
    PRIVATE
        InpxWebReaderApp
)

target_compile_features(InpxWebReaderFb2Fuzzer PRIVATE cxx_std_20)
target_compile_options(InpxWebReaderFb2Fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)
target_link_options(InpxWebReaderFb2Fuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)


target_compile_features(InpxWebReaderInpxArchiveFuzzer PRIVATE cxx_std_20)
target_compile_options(InpxWebReaderInpxArchiveFuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)
target_link_options(InpxWebReaderInpxArchiveFuzzer PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)
