# File: tests/CMakeLists.txt

# Collect all test_*.cpp files automatically
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "test_*.cpp"
)

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

target_link_libraries(fujinet-nio-tests
    PRIVATE
        fujinet-nio
)

target_include_directories(fujinet-nio-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(fujinet-nio-tests fujinet-nio-native-test)
target_compile_definitions(fujinet-nio-tests
    PRIVATE
        FUJINET_NIO_NATIVE_TEST_RUNNER="$<TARGET_FILE:fujinet-nio-native-test>"
)

add_test(NAME fujinet-nio-tests COMMAND fujinet-nio-tests)

# Python unit tests (tooling). Keep these lightweight so they can run everywhere.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME fujinet-tools-python-tests
        COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_SOURCE_DIR}/py
            ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/py/tests -q
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()
