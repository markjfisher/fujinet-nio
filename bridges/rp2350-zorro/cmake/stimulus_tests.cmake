# Experiment registrations: directory, executable, CTest name.
add_stimulus_test(generator-check test_stimulus stimulus 7 16)
add_stimulus_test(C0-idle test_stimulus_c0 stimulus_c0 7 16)
add_stimulus_test(C1-patterns test_stimulus_c1 stimulus_c1 30 28)
add_stimulus_test(C2-held-active test_stimulus_c2 stimulus_c2 12 1)
add_stimulus_test(C3-sampling-window test_stimulus_c3 stimulus_c3 26 4)
add_stimulus_test(C4-repetition test_stimulus_c4 stimulus_c4 32 20)
add_stimulus_test(C5-width-control test_stimulus_c5 stimulus_c5 10 18)
target_compile_definitions(test_stimulus_c5 PRIVATE
    STIMULUS_OUTPUT_BASE=2 STIMULUS_OUTPUT_PINS=21 STIMULUS_DRIVE_PINS=21 STIMULUS_SET_BASE=2
    STIMULUS_SET_PINS=0 STIMULUS_AS_PIN=18 STIMULUS_IDLE_VALUE=983040
    STIMULUS_USE_DMA=1 STIMULUS_OUTPUT_WORD_COUNT=45 STIMULUS_SHIFTCTRL=524288)
