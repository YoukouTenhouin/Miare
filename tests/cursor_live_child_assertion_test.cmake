execute_process(
    COMMAND "${TEST_EXECUTABLE}" --assert-live-cursor
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET)

if(result STREQUAL "0" OR result STREQUAL "77")
    message(FATAL_ERROR "destroying a transaction with a live cursor did not assert")
endif()

message(STATUS "debug cursor lifetime assertion terminated the probe as expected")
