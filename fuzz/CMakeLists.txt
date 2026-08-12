add_executable(miare_format_fuzz format_fuzz.cpp)
target_link_libraries(miare_format_fuzz PRIVATE miare::miare)
target_compile_options(
    miare_format_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(
    miare_format_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(miare_public_history_fuzz public_history_fuzz.cpp)
target_link_libraries(miare_public_history_fuzz PRIVATE miare::miare)
target_compile_options(
    miare_public_history_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(
    miare_public_history_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
