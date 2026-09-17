if(NOT PICO_RP2040)
    message(FATAL_ERROR "Stimulus requires RP2040")
endif()
add_executable(feasibility_stimulus lab/rp2040/main.c
    lab/rp2040/stimulus_program.c lab/rp2040/stimulus_control.c)
add_dependencies(feasibility_stimulus bridge_validate)
target_include_directories(feasibility_stimulus PRIVATE lab/rp2040 "${DEPS}/apio/include")
target_link_libraries(feasibility_stimulus PRIVATE pico_stdlib hardware_pio hardware_clocks)
target_compile_options(feasibility_stimulus PRIVATE -Wall -Wextra -Werror)
pico_enable_stdio_uart(feasibility_stimulus 0)
pico_enable_stdio_usb(feasibility_stimulus 1)
# No flash identification/boot-stage assumptions; ROM loads this image into SRAM.
pico_set_binary_type(feasibility_stimulus no_flash)
pico_add_extra_outputs(feasibility_stimulus)
