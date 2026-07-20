# Per-module unit tests (with golden vectors) arrive alongside the codec in Week 1 S2,
# pulling GoogleTest or doctest via FetchContent at that point. For now a single
# build/link smoke test keeps CI honestly green without adding a test framework before
# there is anything real to test.
add_executable(taut_smoke smoke.cc)
target_link_libraries(taut_smoke PRIVATE taut taut_warnings)
add_test(NAME smoke COMMAND taut_smoke)
