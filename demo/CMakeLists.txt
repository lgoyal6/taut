# Demo binaries (Linux). Relaxed warning set — demos aren't library code.
add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server PRIVATE taut taut_test_warnings)

add_executable(echo_client echo_client.cc)
target_link_libraries(echo_client PRIVATE taut taut_test_warnings)
