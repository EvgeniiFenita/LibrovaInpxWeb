add_executable(InpxWebReader
    main.cpp
)

target_link_libraries(InpxWebReader
    PRIVATE
        InpxWebReaderServer
)

target_compile_features(InpxWebReader PRIVATE cxx_std_20)

set_target_properties(InpxWebReader PROPERTIES OUTPUT_NAME "inpx-web-reader")
