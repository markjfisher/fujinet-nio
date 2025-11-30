# File: tests/CMakeLists.txt

add_executable(fujinet-nio-tests
    test_smoke.cpp
)

target_link_libraries(fujinet-nio-tests
    PRIVATE
        fujinet-nio
)

add_test(NAME fujinet-nio-smoke COMMAND fujinet-nio-tests)
