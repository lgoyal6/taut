# Interface target carrying the project's warning flags. Link it PRIVATE into every
# taut target. Per CLAUDE.md: -Wall -Wextra -Werror, plus a strict extras set.
add_library(taut_warnings INTERFACE)
target_compile_options(taut_warnings INTERFACE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Wnull-dereference
    -Wdouble-promotion
)

# Relaxed set for test targets: third-party framework headers (GoogleTest) trip the
# strict library set, so tests get -Wall -Wextra without -Werror or the pedantic extras.
add_library(taut_test_warnings INTERFACE)
target_compile_options(taut_test_warnings INTERFACE
    -Wall
    -Wextra
)
