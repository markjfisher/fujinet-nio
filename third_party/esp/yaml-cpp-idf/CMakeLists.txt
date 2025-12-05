# Wrapper component to build yaml-cpp under ESP-IDF
#
# We don't use yaml-cpp's own CMake here because that tries to be a full
# CMake project and can interfere with the ESP-IDF toolchain config.

set(YAML_CPP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../yaml-cpp")

# Collect all library sources from yaml-cpp/src
file(GLOB YAML_CPP_SOURCES
    "${YAML_CPP_ROOT}/src/*.cpp"
)

idf_component_register(
    SRCS
        ${YAML_CPP_SOURCES}
    INCLUDE_DIRS
        "${YAML_CPP_ROOT}/include"
    PRIV_INCLUDE_DIRS
        "${YAML_CPP_ROOT}/src"
)

# Make sure we build yaml-cpp as a static lib (no dllexport/dllimport noise)
target_compile_definitions(${COMPONENT_LIB} PUBLIC YAML_CPP_STATIC_DEFINE)
