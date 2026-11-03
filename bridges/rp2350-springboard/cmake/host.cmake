# Build upstream source, including the sole APIO_EMU_IMPL in epio_apio.c.
file(GLOB EPIO_SOURCES CONFIGURE_DEPENDS "${DEPS}/epio/src/*.c")
add_library(epio STATIC ${EPIO_SOURCES})
add_dependencies(epio bridge_validate)
target_include_directories(epio PUBLIC "${DEPS}/epio/include" "${DEPS}/apio/include")
target_compile_definitions(epio PUBLIC APIO_EMULATION=1)
# epio exposes enum-bearing structs; consumers must use the same ABI.
target_compile_options(epio PUBLIC -fshort-enums)

# Tests link the firmware sources they check; none of them touches the SDK.
function(springboard_test name)
    add_executable(test_${name} tests/test_${name}.c ${ARGN})
    add_dependencies(test_${name} bridge_validate)
    target_include_directories(test_${name} PRIVATE src include)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror)
    if(SPRINGBOARD_SANITIZE)
        target_compile_options(test_${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(test_${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

springboard_test(packet_link src/packet_link.c)
springboard_test(sbnio_abi)
springboard_test(bus_write)
# The read engine's PIO program in epio, with the firmware's reply marker.
springboard_test(pio_rearm src/pio_rearm_program.c)
target_link_libraries(test_pio_rearm PRIVATE epio)
target_compile_definitions(test_pio_rearm PRIVATE SB_PIO_REPLY_MARKER=1)
add_test(NAME tooling COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tooling.py")
add_test(NAME setup COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup.py")
add_test(NAME pio_policy COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_pio_policy.py")
