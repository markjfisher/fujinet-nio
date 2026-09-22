# This target is a deliberately isolated Story 2.3 endpoint.  Its cache
# variables make it possible to reproduce a lab experiment without changing
# the production bridge configuration.
set(LINK_DEFAULT_SCENARIO "0" CACHE STRING
    "Story 2.3 lab scenario number (0 through 9)")
set(LINK_SPI_BAUD_HZ "1000000" CACHE STRING
    "SPI clock for the Story 2.3 lab")

# Each bench signal can be remapped without editing the firmware source.
foreach(pin SCK MOSI MISO CS READY DATA_AVAILABLE)
    set(LINK_RP_${pin}_PIN "" CACHE STRING
        "Override RP2350 ${pin} pin for link lab")
endforeach()

if(NOT LINK_DEFAULT_SCENARIO MATCHES "^[0-9]$" OR
        NOT LINK_SPI_BAUD_HZ MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "Link scenario must be 0..9 and SPI baud must be positive")
endif()

add_executable(link_rp2350
    lab/rp2350-link/main.c
    lab/link-common/link_protocol.c
)

add_dependencies(link_rp2350 bridge_validate)
target_include_directories(link_rp2350 PRIVATE lab/link-common)
target_link_libraries(link_rp2350 PRIVATE pico_stdlib hardware_spi hardware_gpio)
target_compile_options(link_rp2350 PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(link_rp2350 PRIVATE
    LINK_DEFAULT_SCENARIO=${LINK_DEFAULT_SCENARIO}
    LINK_SPI_BAUD_HZ=${LINK_SPI_BAUD_HZ})

foreach(pin SCK MOSI MISO CS READY DATA_AVAILABLE)
    if(NOT LINK_RP_${pin}_PIN STREQUAL "")
        if(NOT LINK_RP_${pin}_PIN MATCHES "^[0-9]+$")
            message(FATAL_ERROR "LINK_RP_${pin}_PIN must be a GPIO number")
        endif()
        target_compile_definitions(link_rp2350 PRIVATE LINK_RP_${pin}_PIN=${LINK_RP_${pin}_PIN})
    endif()
endforeach()

# The Core2350B uses USB for the runner protocol and starts from SRAM on every
# lab run, leaving the board's normal flash firmware untouched.
pico_enable_stdio_uart(link_rp2350 0)
pico_enable_stdio_usb(link_rp2350 1)
pico_set_binary_type(link_rp2350 no_flash)
pico_add_extra_outputs(link_rp2350)
