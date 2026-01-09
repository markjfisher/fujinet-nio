# File: tests/CMakeLists.txt

# Collect all test_*.cpp files automatically
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "test_*.cpp"
)

add_executable(fujinet-nio-tests
    run_main.cpp       # contains doctest main
    ${TEST_SOURCES}    # test_smoke.cpp, test_fujipacket.cpp, etc.
)

target_link_libraries(fujinet-nio-tests
    PRIVATE
        fujinet-nio
)

add_test(NAME fujinet-nio-tests COMMAND fujinet-nio-tests)

# Python unit tests (tooling). Keep these lightweight so they can run everywhere.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME fujinet-tools-python-tests
        COMMAND ${CMAKE_COMMAND} -E env
            PYTHONPATH=${CMAKE_SOURCE_DIR}/py
            ${Python3_EXECUTABLE} -m unittest -q fujinet_tools.test_bbc_dfs
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()
