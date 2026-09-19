# Experiment registrations: directory, executable, CTest name.
add_stimulus_test(generator-check test_stimulus stimulus 7 16)
add_stimulus_test(C0-idle test_stimulus_c0 stimulus_c0 7 16)
add_stimulus_test(C1-patterns test_stimulus_c1 stimulus_c1 30 28)
add_stimulus_test(C2-held-active test_stimulus_c2 stimulus_c2 12 1)
add_stimulus_test(C3-sampling-window test_stimulus_c3 stimulus_c3 26 4)
