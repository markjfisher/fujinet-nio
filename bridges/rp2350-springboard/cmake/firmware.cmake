# The Springboard ROM is built elsewhere (Palm apps plus the Handspring card
# header); without one the firmware serves an erased image.
set(SPRINGBOARD_ROM_C "${CMAKE_CURRENT_BINARY_DIR}/rom_image.c")
if(SPRINGBOARD_ROM)
    if(NOT EXISTS "${SPRINGBOARD_ROM}")
        message(FATAL_ERROR "SPRINGBOARD_ROM does not exist: ${SPRINGBOARD_ROM}")
    endif()
    add_custom_command(
        OUTPUT "${SPRINGBOARD_ROM_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rom2c.py"
                "${SPRINGBOARD_ROM}" "${SPRINGBOARD_ROM_C}"
        DEPENDS "${SPRINGBOARD_ROM}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rom2c.py")
else()
    message(NOTICE "SPRINGBOARD_ROM is not set: embedding an erased 0xFF placeholder ROM, "
                   "which has no Handspring card header, so a Visor lists nothing. "
                   "Pass -DSPRINGBOARD_ROM=<file> to serve a real module ROM.")
    add_custom_command(
        OUTPUT "${SPRINGBOARD_ROM_C}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rom2c.py"
                --erased "${SPRINGBOARD_ROM_C}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rom2c.py")
endif()

add_executable(sbnio
    src/main.c
    src/bus.c
    src/link.c
    src/packet_link.c
    src/console.c
    src/usb_descriptors.c
    src/pio_rearm_program.c
    src/persistent_reply.S
    "${SPRINGBOARD_ROM_C}")
add_dependencies(sbnio bridge_validate)
target_include_directories(sbnio PRIVATE src include "${DEPS}/apio/include")
target_compile_options(sbnio PRIVATE -Wall -Wextra)
# Core1's stack shares SCRATCH_X with its reply loop (persistent_reply.S), so it
# gets 2 KiB of the 4 KiB bank.
target_compile_definitions(sbnio PRIVATE
    SB_PIO_REPLY_MARKER=1
    PICO_STACK_SIZE=4096 PICO_CORE1_STACK_SIZE=2048
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=1
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=0
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0)
target_link_libraries(sbnio PRIVATE
    pico_stdlib pico_multicore pico_unique_id pico_usb_reset
    hardware_pio hardware_dma hardware_clocks tinyusb_device)
target_link_options(sbnio PRIVATE -Wl,-z,noexecstack)
# GP0 is the Springboard /IRQ: never let the SDK put a UART on it.
pico_enable_stdio_uart(sbnio 0)
pico_enable_stdio_usb(sbnio 0)
pico_set_binary_type(sbnio copy_to_ram)
pico_add_extra_outputs(sbnio)
