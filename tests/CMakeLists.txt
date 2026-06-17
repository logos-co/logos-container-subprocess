# Logos Container Subprocess tests
#
# Exercises the parent-side container directly (launch/terminate/pid/output
# relay/crash detection) using real child processes (/bin/sleep, /bin/sh).
# Qt-free: only the parent library is needed.

add_executable(logos_container_subprocess_tests
    test_subprocess_container.cpp
)

target_link_libraries(logos_container_subprocess_tests PRIVATE
    logos_container_subprocess
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(logos_container_subprocess_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(logos_container_subprocess_tests)
