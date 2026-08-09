file(READ "${PROVIDER_HEADER}" provider_header)

if(provider_header MATCHES "ZSTD_FrameHeader")
    message(FATAL_ERROR
        "providers.hpp uses ZSTD_FrameHeader, which is unavailable in libzstd 1.4.x; "
        "use the cross-version ZSTD_frameHeader compatibility name")
endif()

if(NOT provider_header MATCHES "ZSTD_frameHeader[ ]+header")
    message(FATAL_ERROR "the bounded frame-header validation is missing")
endif()
