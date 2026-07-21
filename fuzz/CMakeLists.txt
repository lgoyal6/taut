# Fuzzers require clang's libFuzzer (-fsanitize=fuzzer), which supplies main().
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "taut: fuzzers require clang; skipping fuzz targets")
    return()
endif()

add_executable(fuzz_decode fuzz_decode.cc)
target_link_libraries(fuzz_decode PRIVATE taut)
target_compile_options(fuzz_decode PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_decode PRIVATE -fsanitize=fuzzer)
target_compile_definitions(fuzz_decode PRIVATE TAUT_FUZZ_PATCH_CRC)
