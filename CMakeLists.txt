# If IDF_PATH is defined, we're being built as an ESP-IDF project (PlatformIO env).
if(DEFINED ENV{IDF_PATH})
    cmake_minimum_required(VERSION 3.16)

    # Tell ESP-IDF to treat third_party/esp/* as components
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/third_party/esp"
    )

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(fujinet-nio)
else()
    cmake_minimum_required(VERSION 3.20)
    include(fujinet_posix.cmake)
endif()
