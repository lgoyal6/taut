include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# Build/link/parse sanity smoke (no framework) — kept as a fast canary.
add_executable(taut_smoke smoke.cc)
target_link_libraries(taut_smoke PRIVATE taut taut_warnings)
add_test(NAME smoke COMMAND taut_smoke)

# Per-module unit tests (GoogleTest). Test targets use the relaxed warning set so the
# framework's headers don't trip the library's -Werror; the library itself stays strict.
add_executable(taut_unit
    unit/crc32c_test.cc
    unit/codec_test.cc
    unit/timers_test.cc
    unit/sim_net_test.cc
    unit/rto_test.cc
    unit/session_test.cc
    unit/sack_test.cc
    unit/rx_test.cc
    unit/flow_test.cc
    unit/swim_test.cc
)
# The event-loop integration test needs epoll → Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(taut_unit PRIVATE unit/loop_test.cc)
endif()
target_link_libraries(taut_unit PRIVATE taut taut_test_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(taut_unit)
