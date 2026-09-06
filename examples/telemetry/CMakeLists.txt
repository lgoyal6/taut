# Standalone consumer: this directory is NOT part of the taut build.
# It is configured on its own against an installed taut, which is the thing
# worth testing - an in-tree add_subdirectory would prove nothing about the
# installed package.
#
#   cmake --install <taut-build> --prefix /tmp/taut-prefix
#   cmake -S examples/telemetry -B /tmp/telemetry -DCMAKE_PREFIX_PATH=/tmp/taut-prefix
#   cmake --build /tmp/telemetry && /tmp/telemetry/telemetry
cmake_minimum_required(VERSION 3.20)
project(taut_telemetry LANGUAGES CXX)

find_package(taut 0.2 REQUIRED)

add_executable(telemetry main.cc)
target_link_libraries(telemetry PRIVATE taut::taut)
target_compile_features(telemetry PRIVATE cxx_std_20)
