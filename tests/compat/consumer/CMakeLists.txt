cmake_minimum_required(VERSION 3.24)
project(taut_compat_consumer LANGUAGES CXX)

find_package(taut 0.2 REQUIRED CONFIG)
if(NOT taut_VERSION STREQUAL EXPECTED_TAUT_VERSION)
    message(FATAL_ERROR
        "found taut ${taut_VERSION}, expected ${EXPECTED_TAUT_VERSION}")
endif()

add_executable(taut_compat_consumer main.cc)
target_link_libraries(taut_compat_consumer PRIVATE taut::taut)
target_compile_definitions(taut_compat_consumer PRIVATE
    TAUT_EXPECTED_VERSION="${taut_VERSION}")
