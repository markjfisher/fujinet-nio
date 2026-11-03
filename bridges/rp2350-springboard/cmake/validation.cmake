# Always run before consumers, including incremental builds with no changed objects.
add_custom_target(bridge_validate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/bootstrap.py"
        --mode "${BRIDGE_MODE}" --check ${SDK_ARGS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_pio_policy.py"
    COMMENT "Validating pinned dependencies and first-party PIO policy"
    VERBATIM)
