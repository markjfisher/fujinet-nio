# If IDF_PATH is defined, we're being built as an ESP-IDF project (PlatformIO env).
if(DEFINED ENV{IDF_PATH})
    cmake_minimum_required(VERSION 3.16)

    # Configure yaml-cpp to only build the core library, no tests/tools/contrib.
    set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "Disable yaml-cpp tests"   FORCE)
    set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "Disable yaml-cpp tools"   FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "Disable yaml-cpp contrib" FORCE)
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Build yaml-cpp as static" FORCE)

    # Bring in yaml-cpp as a normal CMake subproject.
    add_subdirectory(third_party/yaml-cpp)

    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(fujinet-nio)
else()
    cmake_minimum_required(VERSION 3.20)
    include(fujinet_posix.cmake)
endif()
