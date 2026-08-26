cmake_minimum_required(VERSION 3.24)
project(taut LANGUAGES CXX VERSION 0.2.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)      # defines interface target: taut_warnings
include(Sanitizers)    # applies -fsanitize globally when TAUT_SANITIZE is set

# Pulled in by another project (FetchContent, add_subdirectory), taut should
# build the library and nothing else: a consumer has no use for our tests or
# demo binaries, and building them would drag epoll and libFuzzer requirements
# into their configure step.
if(PROJECT_IS_TOP_LEVEL)
    set(TAUT_DEFAULT_EXTRAS ON)
else()
    set(TAUT_DEFAULT_EXTRAS OFF)
endif()

option(TAUT_BUILD_TESTS "Build taut tests" ${TAUT_DEFAULT_EXTRAS})
option(TAUT_BUILD_DEMOS "Build the echo and SWIM mesh demo binaries" ${TAUT_DEFAULT_EXTRAS})
option(TAUT_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(TAUT_INSTALL "Generate install and package-config rules" ${TAUT_DEFAULT_EXTRAS})

# The core library. CORE modules (codec, window, rto, timers, rx, flow, swim, loop)
# are added source-by-source through the PLAN §2 module loop - each behind its own
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
target_include_directories(taut
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(taut PUBLIC cxx_std_20)
target_link_libraries(taut PRIVATE taut_warnings)

# One source of truth for the version. taut::version() used to carry its own
# hardcoded string, which had already drifted from both the project version and
# the released tag; now it cannot.
target_compile_definitions(taut PRIVATE TAUT_VERSION_STRING="${PROJECT_VERSION}")

# Consumers link taut::taut, in-tree and installed alike.
add_library(taut::taut ALIAS taut)

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

# Demo binaries (echo_server/echo_client) use epoll, so Linux only.
if(TAUT_BUILD_DEMOS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(demo)
endif()

# SWIM mesh demo (feat/swim): runs entirely on the in-process SimNet + virtual clock, so it is
# portable (no epoll). Relaxed warning set - demos aren't library code.
if(TAUT_BUILD_DEMOS)
    add_executable(mesh_node demo/mesh_node.cc)
    target_link_libraries(mesh_node PRIVATE taut taut_test_warnings)
endif()

if(TAUT_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# ---- Benchmarks (§7) - opt-in; not part of the default dev/CI loop ---------------------
# taut vs kernel-TCP vs ENet latency matrix. Off by default (ENet FetchContent needs network
# at configure time; sanitizers would corrupt the numbers). Configure a release build with
# -DTAUT_BUILD_BENCH=ON to build bench/latency_bench, tcp_baseline, enet_baseline.
option(TAUT_BUILD_BENCH "Build benchmark binaries (taut/TCP/ENet latency matrix)" OFF)
if(TAUT_BUILD_BENCH AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(bench)
endif()

# --------------------------------------------------------------- install ---
#
# Two supported ways to consume taut, both landing on the same taut::taut
# target:
#
#   FetchContent_Declare(taut GIT_REPOSITORY https://github.com/lgoyal6/taut.git
#                             GIT_TAG v0.2.1)
#   FetchContent_MakeAvailable(taut)
#   target_link_libraries(app PRIVATE taut::taut)
#
#   cmake --install build --prefix /usr/local   # then, from anywhere:
#   find_package(taut 0.2 REQUIRED)
#   target_link_libraries(app PRIVATE taut::taut)
#
# Note for consumers: the reliable-UDP transport and epoll loop are Linux-only.
# On other platforms the codec, CRC, RTO estimator, timers, SWIM and the
# in-process SimNet still build, which is enough to unit-test against.
if(TAUT_INSTALL)
    include(CMakePackageConfigHelpers)

    # taut_warnings is a build-time-only interface target, but the exported
    # target still names it, so it has to be installed or find_package fails
    # on a target the consumer has never heard of.
    install(TARGETS taut taut_warnings
            EXPORT tautTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/taut DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT tautTargets
            FILE tautTargets.cmake
            NAMESPACE taut::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taut)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/tautConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tautConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/tautConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taut)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/tautConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/tautConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/taut)
endif()
