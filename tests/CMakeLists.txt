find_package(Threads REQUIRED)

add_compile_options(
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)

add_executable(miare_public_primitives_test public_primitives_test.cpp)
target_link_libraries(miare_public_primitives_test PRIVATE miare::miare)
add_test(NAME miare.public_primitives COMMAND miare_public_primitives_test)

add_executable(miare_minimal_consumer minimal_consumer.cpp)
target_link_libraries(miare_minimal_consumer PRIVATE miare::miare)
add_test(NAME miare.minimal_consumer COMMAND miare_minimal_consumer)

add_executable(miare_durable_file_test durable_file_test.cpp)
target_link_libraries(miare_durable_file_test PRIVATE miare::miare)
add_test(NAME miare.durable_file COMMAND miare_durable_file_test)

add_executable(miare_provider_test provider_test.cpp)
target_link_libraries(miare_provider_test PRIVATE miare::miare)
add_test(NAME miare.provider COMMAND miare_provider_test)

add_executable(miare_database_format_test database_format_test.cpp)
target_link_libraries(miare_database_format_test PRIVATE miare::miare)
add_test(NAME miare.database_format COMMAND miare_database_format_test)

add_executable(miare_database_lifecycle_test database_lifecycle_test.cpp)
target_link_libraries(
    miare_database_lifecycle_test PRIVATE miare::miare Threads::Threads)
add_test(NAME miare.database_lifecycle COMMAND miare_database_lifecycle_test)

add_executable(miare_exact_transaction_test exact_transaction_test.cpp)
target_link_libraries(
    miare_exact_transaction_test PRIVATE miare::miare Threads::Threads)
add_test(NAME miare.exact_transaction COMMAND miare_exact_transaction_test)
set_tests_properties(miare.exact_transaction PROPERTIES TIMEOUT 10)

add_executable(
    miare_ordered_keyspace_growth_test ordered_keyspace_growth_test.cpp)
target_link_libraries(
    miare_ordered_keyspace_growth_test PRIVATE miare::miare Threads::Threads)
add_test(
    NAME miare.ordered_keyspace_growth
    COMMAND miare_ordered_keyspace_growth_test)
set_tests_properties(miare.ordered_keyspace_growth PROPERTIES TIMEOUT 30)
if(MIARE_BUILD_QUALIFICATION_TESTS)
    add_test(
        NAME miare.ordered_keyspace_qualification
        COMMAND miare_ordered_keyspace_growth_test --qualification)
    set_tests_properties(
        miare.ordered_keyspace_qualification
        PROPERTIES
            LABELS qualification
            TIMEOUT 21600)
endif()
add_test(
    NAME miare.exact_transaction_live_child_assertion
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:miare_exact_transaction_test>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/live_child_assertion_test.cmake)
set_tests_properties(
    miare.exact_transaction_live_child_assertion PROPERTIES TIMEOUT 10)

add_test(
    NAME miare.zstd_header_compat
    COMMAND
        ${CMAKE_COMMAND}
        -DPROVIDER_HEADER=${PROJECT_SOURCE_DIR}/include/miare/detail/providers.hpp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/zstd_header_compat_test.cmake)
