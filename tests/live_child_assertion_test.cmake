execute_process(
    COMMAND "${TEST_EXECUTABLE}" --assert-live-child
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET)

if(result STREQUAL "0")
    message(FATAL_ERROR "destroying a database with a live child did not assert")
endif()

if(NOT result STREQUAL "77")
    message(STATUS "debug lifetime assertion terminated the probe as expected")
endif()
