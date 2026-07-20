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

# The core library. CORE modules (codec, window, rto, timers, rx, flow, swim, loop)
# are added source-by-source through the PLAN §2 module loop — each behind its own
# design brief + comprehension gate. For now it carries only version.cc plus the
# public API sketch headers (Week 1 S1).
add_library(taut STATIC
    src/version.cc
    src/crc32c.cc
)
target_include_directories(taut PUBLIC include)
target_compile_features(taut PUBLIC cxx_std_20)
target_link_libraries(taut PRIVATE taut_warnings)

if(TAUT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
