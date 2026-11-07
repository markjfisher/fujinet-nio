# File: tests/CMakeLists.txt

# Collect all test_*.cpp files automatically
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "test_*.cpp"
)
# HotSync's tests need its sources (FN_WITH_HOTSYNC).
if(NOT FN_WITH_HOTSYNC)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "/test_hotsync[^/]*\\.cpp$")
endif()

# Story 1.9 host harness: not part of the production fujinet-nio library.
add_executable(fujinet-nio-native-test
    native_test_runner.cpp
    directory_packet_io.cpp
)
target_include_directories(fujinet-nio-native-test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(fujinet-nio-native-test
    PRIVATE
        fujinet-nio
)

# Story 2.3 integration peer: it exposes no SIO protocol of its own.  The
# emulator reaches the production NetSIO ingress and AtariSioNioFrontend;
# this process supplies only a deterministic real-core VirtualDevice for the
# independent core-dispatch fixture.
add_executable(fujinet-nio-atari-netsio-projection
    atari_netsio_projection_runner.cpp
)
target_link_libraries(fujinet-nio-atari-netsio-projection
    PRIVATE
        fujinet-nio
)

# Story 2.4 public cc65 API guest route.  The production ClockDevice runs
# against a deterministic test-only POSIX clock; its Atari SIO ingress remains
# the normal NetSIO/AtariSioNioFrontend implementation.
add_executable(fujinet-nio-atari-netsio-clock-acceptance
    atari_netsio_clock_acceptance_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/posix/time.cpp
)
target_compile_definitions(fujinet-nio-atari-netsio-clock-acceptance
    PRIVATE
        FN_POSIX_TEST_CLOCK=1
)
target_link_libraries(fujinet-nio-atari-netsio-clock-acceptance
    PRIVATE
        fujinet-nio
)

add_executable(fujinet-nio-tests
    run_main.cpp       # contains doctest main
    ${TEST_SOURCES}    # test_smoke.cpp, test_fujipacket.cpp, etc.
    directory_packet_io.cpp
    # Console implementation files live in src/app/ (POSIX app), but are useful to unit test.
    # Pull them into the test binary so we can exercise help/dispatch/fs-shell behavior.
    ${CMAKE_SOURCE_DIR}/src/app/console_parse.cpp
    ${CMAKE_SOURCE_DIR}/src/app/console_commands.cpp
    ${CMAKE_SOURCE_DIR}/src/app/console_fs_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/app/console_engine.cpp
)

# Wire-level golden vectors for the Atari SIO/NIO frontends. They are owned by
# this repository (the firmware defines the protocol); the workspace and
# emulator tests read them from here.
target_compile_definitions(fujinet-nio-tests
    PRIVATE
        FN_ATARI_SIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/atari-sio"
        FN_XEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/xex-boot"
)

# Compile the platform clock again only for the unit-test executable. These
# definitions satisfy its clock references before the production static archive
# is searched; the archive/app/native runner retain the normal system clock.
target_sources(fujinet-nio-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/platform/posix/time.cpp)
target_compile_definitions(fujinet-nio-tests PRIVATE FN_POSIX_TEST_CLOCK=1)

target_link_libraries(fujinet-nio-tests
    PRIVATE
        fujinet-nio
)

target_include_directories(fujinet-nio-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(fujinet-nio-tests fujinet-nio-native-test fujinet-nio-atari-netsio-projection
    fujinet-nio-atari-netsio-clock-acceptance)
target_compile_definitions(fujinet-nio-tests
    PRIVATE
        FUJINET_NIO_NATIVE_TEST_RUNNER="$<TARGET_FILE:fujinet-nio-native-test>"
)

add_test(NAME fujinet-nio-tests COMMAND fujinet-nio-tests)

set(CLOCK_ISOLATION_APP_ARG)
if(TARGET fujinet-nio-posix)
    add_dependencies(fujinet-nio-tests fujinet-nio-posix)
    set(CLOCK_ISOLATION_APP_ARG -DPRODUCTION_APP=$<TARGET_FILE:fujinet-nio-posix>)
endif()
add_test(NAME posix-clock-test-isolation
    COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DPRODUCTION_LIBRARY=$<TARGET_FILE:fujinet-nio>
        -DNATIVE_RUNNER=$<TARGET_FILE:fujinet-nio-native-test>
        -DUNIT_TESTS=$<TARGET_FILE:fujinet-nio-tests>
        ${CLOCK_ISOLATION_APP_ARG}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_clock_test_isolation.cmake
)

# Python unit tests (tooling). Keep these lightweight so they can run everywhere.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME esp32-usb-cdc-channel
        COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_esp32_usb_cdc_channel.py)
    set_tests_properties(esp32-usb-cdc-channel PROPERTIES TIMEOUT 150)
    if(FN_WITH_HOTSYNC AND FN_BUILD_FUJIBUS_PTY AND TARGET fujinet-nio-posix)
        add_test(NAME hotsync-stalled-netsync-shutdown
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/hotsync_shutdown_integration.py
                    $<TARGET_FILE:fujinet-nio-posix>)
        set_tests_properties(hotsync-stalled-netsync-shutdown PROPERTIES TIMEOUT 20)
    endif()
    add_test(
        NAME fujinet-tools-python-tests
        COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_SOURCE_DIR}/py
            ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/py/tests -q
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

# Compile flight recording separately: the normal suite tests production defaults,
# and this executable tests the identical capture/recovery cases with recording on.
add_executable(atari-sio-flight-tests
    run_main.cpp
    test_atari_sio_transaction_boundary.cpp
    test_atari_sio_diagnostics.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/diagnostic_atari_sio_provider.cpp
)
target_include_directories(atari-sio-flight-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
target_compile_features(atari-sio-flight-tests PRIVATE cxx_std_20)
target_compile_definitions(atari-sio-flight-tests PRIVATE
    FN_PLATFORM_POSIX=1 FN_ATARI_SIO_FLIGHT=1)
add_test(NAME atari-sio-flight-tests COMMAND atari-sio-flight-tests)

# Real production Visor implementation against link-time USB mocks; never enumerates
# hardware and never links libusb. Enabled only with the explicit host profile.
if(FN_BUILD_VISOR_USB_HOST)
    add_executable(visor-usb-tests
        run_main.cpp
        visor_usb/test_protocol.cpp
        visor_usb/test_host.cpp
        visor_usb/mock_usb.cpp
        # Exercise the real console/PTY path alongside the USB mocks.
        ${CMAKE_SOURCE_DIR}/src/app/console_parse.cpp
        ${CMAKE_SOURCE_DIR}/src/app/console_commands.cpp
        ${CMAKE_SOURCE_DIR}/src/app/console_fs_shell.cpp
        ${CMAKE_SOURCE_DIR}/src/app/console_engine.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/posix/visor_usb/host.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/posix/visor_usb/ingress.cpp
        ${CMAKE_SOURCE_DIR}/src/lib/visor_usb/protocol.cpp)
    target_include_directories(visor-usb-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${LIBUSB_INCLUDE_DIRS})
    # Link the archive without its libusb dependency: every USB entry point is
    # supplied by mock_usb.cpp. Keep all other existing platform dependencies.
    get_target_property(VISOR_TEST_DEPS fujinet-nio LINK_LIBRARIES)
    list(REMOVE_ITEM VISOR_TEST_DEPS PkgConfig::LIBUSB)
    add_dependencies(visor-usb-tests fujinet-nio)
    target_include_directories(visor-usb-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(visor-usb-tests PRIVATE FN_PLATFORM_POSIX)
    target_link_libraries(visor-usb-tests PRIVATE $<TARGET_FILE:fujinet-nio> ${VISOR_TEST_DEPS})
    add_test(NAME visor-usb-tests COMMAND visor-usb-tests)
endif()
if(FN_BUILD_VISOR_USB_HOST AND Python3_Interpreter_FOUND)
    add_test(NAME visor-usb-source-ownership
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/visor_usb/test_generation.py)
endif()
