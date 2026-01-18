# __GENERATED_COMMENT__

project(fujinet_nio
    VERSION 0.1.1
    LANGUAGES C CXX
)

# Configure yaml-cpp to only build the core library, no tests/tools/contrib.
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "Disable yaml-cpp tests"   FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "Disable yaml-cpp tools"   FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "Disable yaml-cpp contrib" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "Build yaml-cpp as static" FORCE)

# Bring in yaml-cpp (third-party YAML library)
add_subdirectory(third_party/yaml-cpp)

# Options
option(FN_BUILD_POSIX_APP     "Build POSIX console application"           ON)
option(FN_BUILD_TESTS         "Build unit tests"                          ON)
option(FN_WITH_CURL           "Enable libcurl-backed HTTP/HTTPS on POSIX" ON)

# Build options, used in build_profile.cpp, these need reflecting in the target_compile_definitions below
# These can then be used as flags for the build type in cmake via CMakePresets.json
option (FN_BUILD_ATARI_SIO    "Build for Atari SIO via GPIO (ESP32)"  OFF)
option (FN_BUILD_ATARI_PTY    "Build for Atari SIO over PTY (POSIX)"  OFF)
option (FN_BUILD_ATARI_NETSIO "Build for Atari SIO over NetSIO/UDP (POSIX)" OFF)
option (FN_BUILD_FUJIBUS_PTY  "Build for FUJIBUS PTY profile"         OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable folders in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# --------------------------------------------------
# Library target: fujinet-nio
# --------------------------------------------------
add_library(fujinet-nio)

# --------------------------------------------------
# CURL
# --------------------------------------------------
if(FN_WITH_CURL)
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_compile_definitions(fujinet-nio PUBLIC FN_WITH_CURL=1)
    target_link_libraries(fujinet-nio PRIVATE CURL::libcurl)
  else()
    message(WARNING "FN_WITH_CURL=ON but libcurl not found; disabling HTTP/HTTPS backend.")
    target_compile_definitions(fujinet-nio PUBLIC FN_WITH_CURL=0)
  endif()
else()
  target_compile_definitions(fujinet-nio PUBLIC FN_WITH_CURL=0)
endif()

# --------------------------------------------------
# Build Flags
# --------------------------------------------------
target_compile_definitions(fujinet-nio
    PUBLIC
        FN_PLATFORM_POSIX               # always true in this toolchain
        $<$<CONFIG:Debug>:FN_DEBUG>
        $<$<BOOL:${FN_BUILD_ATARI_SIO}>:FN_BUILD_ATARI_SIO>
        $<$<BOOL:${FN_BUILD_ATARI_PTY}>:FN_BUILD_ATARI_PTY>
        $<$<BOOL:${FN_BUILD_ATARI_NETSIO}>:FN_BUILD_ATARI_NETSIO>
        $<$<BOOL:${FN_BUILD_FUJIBUS_PTY}>:FN_BUILD_FUJIBUS_PTY>
        # ADD MORE BUILD OPTIONS AS WE DEVELOP THEM HERE
)

target_sources(fujinet-nio
    PRIVATE
# __TARGET_SOURCES_START__
# __TARGET_SOURCES_END__
        third_party/cjson/cJSON.c
)


target_include_directories(fujinet-nio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson
)

target_compile_features(fujinet-nio PUBLIC cxx_std_20)

# Link yaml-cpp
target_link_libraries(fujinet-nio
    PUBLIC
        yaml-cpp
)

# --------------------------------------------------
# POSIX app (Linux/macOS/Windows console)
# --------------------------------------------------
if(FN_BUILD_POSIX_APP)
    add_executable(fujinet-nio-posix
        src/app/main_posix.cpp
    )

    target_sources(fujinet-nio-posix
        PRIVATE
# __POSIX_APP_SOURCES_START__
# __POSIX_APP_SOURCES_END__
    )

    target_link_libraries(fujinet-nio-posix
        PRIVATE
            fujinet-nio
    )

    set_target_properties(fujinet-nio-posix PROPERTIES
        OUTPUT_NAME "fujinet-nio"
    )

    # Copy a small runner script next to the built binary (for "reboot" restarts on EX_TEMPFAIL=75).
    add_custom_command(TARGET fujinet-nio-posix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/distfiles/run-fujinet-nio
            $<TARGET_FILE_DIR:fujinet-nio-posix>/run-fujinet-nio
        COMMAND /bin/chmod +x
            $<TARGET_FILE_DIR:fujinet-nio-posix>/run-fujinet-nio
        VERBATIM
    )
endif()

# --------------------------------------------------
# Tests (using CTest/doctest)
# --------------------------------------------------
if(FN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
