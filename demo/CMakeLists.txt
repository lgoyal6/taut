# Demo binaries (Linux). Relaxed warning set - demos aren't library code.
add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server PRIVATE taut taut_test_warnings)

add_executable(echo_client echo_client.cc)
target_link_libraries(echo_client PRIVATE taut taut_test_warnings)

# File-transfer reliability demo (Week 2 checkpoint): send_file / recv_file move a file over
# class 2 and print sha256 at both ends. Header-only helpers (sha256.h, file_xfer.h) are
# included from this directory.
add_executable(send_file send_file.cc)
target_link_libraries(send_file PRIVATE taut taut_test_warnings)

add_executable(recv_file recv_file.cc)
target_link_libraries(recv_file PRIVATE taut taut_test_warnings)
