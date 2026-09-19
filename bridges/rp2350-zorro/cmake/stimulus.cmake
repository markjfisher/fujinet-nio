if(NOT PICO_RP2040)
    message(FATAL_ERROR "Stimulus requires RP2040")
endif()
set(STIMULUS_EXPERIMENT "" CACHE STRING "Experiment stimulus source directory")
set(STIMULUS_TARGET "" CACHE STRING "Experiment firmware target")
set(STIMULUS_WORDS "7" CACHE STRING "PIO instruction words for selected stimulus")
set(STIMULUS_SAMPLE_COUNT "16" CACHE STRING "Assertions in selected stimulus")
set(STIMULUS_STORAGE "ram" CACHE STRING "Stimulus storage: ram or flash")
set(STIMULUS_OUTPUT_BASE "2" CACHE STRING "First contiguous RP2040 PIO output GPIO")
set(STIMULUS_OUTPUT_PINS "4" CACHE STRING "Number of pins driven by PIO OUT")
set(STIMULUS_DRIVE_PINS "5" CACHE STRING "Number of contiguous GPIOs driven by PIO")
set(STIMULUS_SET_BASE "6" CACHE STRING "PIO SET pin base")
set(STIMULUS_SET_PINS "1" CACHE STRING "PIO SET pin count")
set(STIMULUS_AS_PIN "6" CACHE STRING "Active-low /AS GPIO within output range")
set(STIMULUS_IDLE_VALUE "16" CACHE STRING "Relative output value used while inactive")
set(STIMULUS_USE_DMA "0" CACHE STRING "Feed PIO TX FIFO through DMA")
set(STIMULUS_OUTPUT_WORD_COUNT "0" CACHE STRING "DMA output words for selected stimulus")
set(STIMULUS_SHIFTCTRL "0" CACHE STRING "PIO shift-control register image")
set(STIMULUS_HZ "100000" CACHE STRING "PIO state-machine frequency in Hz")
if(NOT STIMULUS_EXPERIMENT MATCHES "^[A-Za-z0-9_-]+$" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/feasibility/${STIMULUS_EXPERIMENT}/src/stimulus_program.c")
    message(FATAL_ERROR "Select an experiment directory containing src/stimulus_program.c")
endif()
if(NOT STIMULUS_TARGET MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Select a stimulus firmware target")
endif()
if(NOT STIMULUS_WORDS MATCHES "^[1-9][0-9]*$" OR STIMULUS_WORDS GREATER 32 OR
   NOT STIMULUS_SAMPLE_COUNT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "Stimulus word and sample counts must be positive (PIO words <= 32)")
endif()
if(NOT STIMULUS_STORAGE MATCHES "^(ram|flash)$")
    message(FATAL_ERROR "STIMULUS_STORAGE must be ram or flash")
endif()
if(NOT STIMULUS_OUTPUT_BASE MATCHES "^[0-9]+$" OR
   NOT STIMULUS_OUTPUT_PINS MATCHES "^[1-9][0-9]*$" OR STIMULUS_OUTPUT_PINS GREATER 30 OR
   NOT STIMULUS_DRIVE_PINS MATCHES "^[1-9][0-9]*$" OR STIMULUS_DRIVE_PINS GREATER 30 OR
   NOT STIMULUS_SET_BASE MATCHES "^[0-9]+$" OR NOT STIMULUS_SET_PINS MATCHES "^[0-9]+$" OR
   NOT STIMULUS_AS_PIN MATCHES "^[0-9]+$" OR NOT STIMULUS_IDLE_VALUE MATCHES "^[0-9]+$" OR
   NOT STIMULUS_USE_DMA MATCHES "^[01]$" OR NOT STIMULUS_OUTPUT_WORD_COUNT MATCHES "^[0-9]+$" OR
   NOT STIMULUS_SHIFTCTRL MATCHES "^[0-9]+$" OR NOT STIMULUS_HZ MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "Invalid stimulus GPIO/DMA configuration")
endif()
math(EXPR STIMULUS_OUTPUT_LAST "${STIMULUS_OUTPUT_BASE} + ${STIMULUS_DRIVE_PINS} - 1")
if(STIMULUS_OUTPUT_LAST GREATER 29 OR STIMULUS_AS_PIN LESS STIMULUS_OUTPUT_BASE OR
   STIMULUS_AS_PIN GREATER STIMULUS_OUTPUT_LAST OR
   (STIMULUS_USE_DMA STREQUAL "1" AND STIMULUS_OUTPUT_WORD_COUNT EQUAL 0))
    message(FATAL_ERROR "Stimulus output range, /AS pin or DMA word count is invalid")
endif()
add_executable(${STIMULUS_TARGET} lab/rp2040/main.c
    tests/feasibility/${STIMULUS_EXPERIMENT}/src/stimulus_program.c lab/rp2040/stimulus_control.c)
add_dependencies(${STIMULUS_TARGET} bridge_validate)
target_include_directories(${STIMULUS_TARGET} PRIVATE lab/rp2040 "${DEPS}/apio/include")
target_link_libraries(${STIMULUS_TARGET} PRIVATE pico_stdlib hardware_pio hardware_clocks hardware_dma)
target_compile_options(${STIMULUS_TARGET} PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(${STIMULUS_TARGET} PRIVATE STIMULUS_WORDS=${STIMULUS_WORDS}
    STIMULUS_SAMPLE_COUNT=${STIMULUS_SAMPLE_COUNT}
    STIMULUS_OUTPUT_BASE=${STIMULUS_OUTPUT_BASE}
    STIMULUS_OUTPUT_PINS=${STIMULUS_OUTPUT_PINS}
    STIMULUS_DRIVE_PINS=${STIMULUS_DRIVE_PINS}
    STIMULUS_SET_BASE=${STIMULUS_SET_BASE}
    STIMULUS_SET_PINS=${STIMULUS_SET_PINS}
    STIMULUS_AS_PIN=${STIMULUS_AS_PIN}
    STIMULUS_IDLE_VALUE=${STIMULUS_IDLE_VALUE}
    STIMULUS_USE_DMA=${STIMULUS_USE_DMA}
    STIMULUS_OUTPUT_WORD_COUNT=${STIMULUS_OUTPUT_WORD_COUNT}
    STIMULUS_SHIFTCTRL=${STIMULUS_SHIFTCTRL}
    STIMULUS_HZ=${STIMULUS_HZ})
pico_enable_stdio_uart(${STIMULUS_TARGET} 0)
pico_enable_stdio_usb(${STIMULUS_TARGET} 1)
if(STIMULUS_STORAGE STREQUAL "ram")
    # No flash identification/boot-stage assumptions; ROM loads this image into SRAM.
    pico_set_binary_type(${STIMULUS_TARGET} no_flash)
endif()
pico_add_extra_outputs(${STIMULUS_TARGET})
