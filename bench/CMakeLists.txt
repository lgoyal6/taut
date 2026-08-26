# Benchmark binaries (§7). Enabled with -DTAUT_BUILD_BENCH=ON (see root CMakeLists); Linux
# only (real sockets + ENet). Kept out of the default dev/CI build so the core loop stays
# fast and network-free. Run these from a *release* build - sanitizers would dominate the
# latency numbers.
include(FetchContent)

# Header-only shared plumbing. Include root so `#include "bench/common.h"` resolves.
add_library(bench_common INTERFACE)
target_include_directories(bench_common INTERFACE ${CMAKE_SOURCE_DIR})
target_compile_features(bench_common INTERFACE cxx_std_20)

# taut load generator + kernel-TCP baseline: our code, held to the full strict warning set.
add_executable(latency_bench latency_bench.cc)
target_link_libraries(latency_bench PRIVATE taut bench_common taut_warnings)

add_executable(tcp_baseline tcp_baseline.cc)
target_link_libraries(tcp_baseline PRIVATE bench_common taut_warnings)

# ENet baseline (vendored via FetchContent). Requires network at configure time; toggle off
# with -DTAUT_BENCH_ENET=OFF if the fetch is unavailable (the matrix still runs taut + TCP).
option(TAUT_BENCH_ENET "Build the ENet baseline (fetches lsalzman/enet)" ON)
if(TAUT_BENCH_ENET)
    set(ENET_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG v1.3.18
    )
    FetchContent_MakeAvailable(enet)
    add_executable(enet_baseline enet_baseline.cc)
    # ENet's headers predate strict C++ warnings; mark them SYSTEM and use the relaxed set.
    target_link_libraries(enet_baseline PRIVATE bench_common enet taut_test_warnings)
    target_include_directories(enet_baseline SYSTEM PRIVATE ${enet_SOURCE_DIR}/include)
endif()
