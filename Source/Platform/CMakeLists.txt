# Platform TUs are compiled into Foundation_Core (avoids Core↔Platform link cycles).
# CMake selects the matching backends; no OHOS sinks yet.

set(FOUNDATION_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if (WIN32)
    target_sources(Foundation_Core PRIVATE
        "${FOUNDATION_PLATFORM_DIR}/LoggingWin32.cpp"
        "${FOUNDATION_PLATFORM_DIR}/MmapWin32.cpp"
    )
elseif (ANDROID)
    target_sources(Foundation_Core PRIVATE
        "${FOUNDATION_PLATFORM_DIR}/LoggingAndroid.cpp"
        "${FOUNDATION_PLATFORM_DIR}/MmapPosix.cpp"
    )
    target_link_libraries(Foundation_Core PUBLIC log android)
else()
    target_sources(Foundation_Core PRIVATE
        "${FOUNDATION_PLATFORM_DIR}/LoggingPosix.cpp"
        "${FOUNDATION_PLATFORM_DIR}/MmapPosix.cpp"
    )
endif()

source_group(TREE "${FOUNDATION_PLATFORM_DIR}" PREFIX "Platform"
    FILES
        "${FOUNDATION_PLATFORM_DIR}/Logging.hpp"
        "${FOUNDATION_PLATFORM_DIR}/LoggingWin32.cpp"
        "${FOUNDATION_PLATFORM_DIR}/LoggingAndroid.cpp"
        "${FOUNDATION_PLATFORM_DIR}/LoggingPosix.cpp"
        "${FOUNDATION_PLATFORM_DIR}/Mmap.hpp"
        "${FOUNDATION_PLATFORM_DIR}/MmapWin32.cpp"
        "${FOUNDATION_PLATFORM_DIR}/MmapPosix.cpp"
)
