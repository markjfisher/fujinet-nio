# Isolated bench image. Native USB is exclusively a host; console is UART0.
set(SPRINGBOARD_LINK "${CMAKE_CURRENT_SOURCE_DIR}/../rp2350-springboard/src")
add_executable(usb_host_probe lab/usb-host/main.c lab/usb-host/probe.c
    "${SPRINGBOARD_LINK}/packet_link.c")
add_dependencies(usb_host_probe bridge_validate)
target_include_directories(usb_host_probe PRIVATE lab/usb-host "${SPRINGBOARD_LINK}")
target_link_libraries(usb_host_probe PRIVATE pico_stdlib tinyusb_host hardware_uart)
target_compile_options(usb_host_probe PRIVATE -Wall -Wextra -Werror)
pico_enable_stdio_usb(usb_host_probe 0)
pico_enable_stdio_uart(usb_host_probe 1)
pico_add_extra_outputs(usb_host_probe)
