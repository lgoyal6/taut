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
