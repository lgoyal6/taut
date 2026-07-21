cmake_minimum_required(VERSION 3.24)
project(taut LANGUAGES CXX VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)      # defines interface target: taut_warnings
include(Sanitizers)    # applies -fsanitize globally when TAUT_SANITIZE is set

option(TAUT_BUILD_TESTS "Build taut tests" ON)
option(TAUT_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)

# The core library. CORE modules (codec, window, rto, timers, rx, flow, swim, loop)
# are added source-by-source through the PLAN §2 module loop — each behind its own
# design brief + comprehension gate. For now it carries only version.cc plus the
# public API sketch headers (Week 1 S1).
add_library(taut STATIC
    src/version.cc
    src/crc32c.cc
    src/codec.cc
    src/timers.cc
    src/sim_net.cc
    src/rto.cc
    src/session.cc
    src/swim.cc
)
target_include_directories(taut PUBLIC include)
target_compile_features(taut PUBLIC cxx_std_20)
target_link_libraries(taut PRIVATE taut_warnings)

# epoll event loop + real UDP transport are Linux-only; the codec/crc stay portable.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(taut PRIVATE
        src/transport.cc
        src/loop.cc
    )
endif()

if(TAUT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Demo binaries (echo_server/echo_client) use epoll → Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(demo)
endif()

# SWIM mesh demo (feat/swim): runs entirely on the in-process SimNet + virtual clock, so it is
# portable (no epoll) and always built. Relaxed warning set — demos aren't library code.
add_executable(mesh_node demo/mesh_node.cc)
target_link_libraries(mesh_node PRIVATE taut taut_test_warnings)

if(TAUT_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# ---- Benchmarks (§7) — opt-in; not part of the default dev/CI loop ---------------------
# taut vs kernel-TCP vs ENet latency matrix. Off by default (ENet FetchContent needs network
# at configure time; sanitizers would corrupt the numbers). Configure a release build with
# -DTAUT_BUILD_BENCH=ON to build bench/latency_bench, tcp_baseline, enet_baseline.
option(TAUT_BUILD_BENCH "Build benchmark binaries (taut/TCP/ENet latency matrix)" OFF)
if(TAUT_BUILD_BENCH AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(bench)
endif()
