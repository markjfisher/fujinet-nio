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
