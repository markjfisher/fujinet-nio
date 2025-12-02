project(fujinet_nio
    VERSION 0.1.1
    LANGUAGES CXX
)

# Options
option(FN_BUILD_POSIX_APP "Build POSIX console application" ON)
option(FN_BUILD_TESTS     "Build unit tests" ON)

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

# For now, it's tiny; we'll add more sources as the IO system grows.
target_sources(fujinet-nio
    PRIVATE
        src/lib/bootstrap.cpp
        src/lib/build_profile.cpp
        src/lib/fuji_bus_packet.cpp
        src/lib/fujinet_core.cpp
        src/lib/fujinet_init.cpp
        src/lib/io_device_manager.cpp
        src/lib/io_service.cpp
        src/lib/routing_manager.cpp
        src/lib/fujibus_transport.cpp
        src/platform/posix/channel_factory.cpp
)

target_include_directories(fujinet-nio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(fujinet-nio PUBLIC cxx_std_20)

# --------------------------------------------------
# POSIX app (Linux/macOS/Windows console)
# --------------------------------------------------
if(FN_BUILD_POSIX_APP)
    add_executable(fujinet-nio-posix
        src/app/main_posix.cpp
    )

    target_link_libraries(fujinet-nio-posix
        PRIVATE
            fujinet-nio
    )

    set_target_properties(fujinet-nio-posix PROPERTIES
        OUTPUT_NAME "fujinet-nio"
    )
endif()

# --------------------------------------------------
# Tests (using CTest/doctest)
# --------------------------------------------------
if(FN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
