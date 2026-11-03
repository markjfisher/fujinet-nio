# SDK discovers this target and does not run its implicit fetch/find workflow.
include(ExternalProject)
set(BRIDGE_PICOTOOL_PREFIX "${CMAKE_BINARY_DIR}/picotool-host")
ExternalProject_Add(bridge_picotool_build
    SOURCE_DIR "${DEPS}/picotool"
    BINARY_DIR "${BRIDGE_PICOTOOL_PREFIX}/build"
    INSTALL_DIR "${BRIDGE_PICOTOOL_PREFIX}"
    CMAKE_ARGS
        "-DCMAKE_INSTALL_PREFIX=${BRIDGE_PICOTOOL_PREFIX}"
        "-DPICO_SDK_PATH=${PICO_SDK_PATH}"
        -DPICOTOOL_NO_LIBUSB=1 -DPICOTOOL_FLAT_INSTALL=1
    BUILD_BYPRODUCTS "${BRIDGE_PICOTOOL_PREFIX}/picotool/picotool"
    EXCLUDE_FROM_ALL TRUE)
add_executable(picotool IMPORTED GLOBAL)
set_target_properties(picotool PROPERTIES IMPORTED_LOCATION "${BRIDGE_PICOTOOL_PREFIX}/picotool/picotool")
add_dependencies(picotool bridge_picotool_build)
