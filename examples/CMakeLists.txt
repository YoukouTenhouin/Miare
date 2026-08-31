add_executable(miare_key_value_example key_value.cpp)
target_link_libraries(miare_key_value_example PRIVATE miare::miare)

if(MIARE_HAS_SODIUM AND MIARE_HAS_ZSTD)
    add_executable(miare_blob_example blob.cpp)
    target_link_libraries(miare_blob_example PRIVATE miare::miare)
endif()

if(MIARE_BUILD_TESTS)
    add_test(NAME miare.example.key_value COMMAND miare_key_value_example)
    if(MIARE_HAS_SODIUM AND MIARE_HAS_ZSTD)
        add_test(NAME miare.example.blob COMMAND miare_blob_example)
    endif()
endif()
