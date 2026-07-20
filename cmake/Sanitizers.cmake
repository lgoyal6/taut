# Global sanitizer wiring. Set TAUT_SANITIZE to a comma-separated list
# (e.g. "address,undefined") and every target in the build is instrumented and linked
# consistently — required for ASan/UBSan to work across the whole process. The `dev`
# preset sets address,undefined; release leaves it empty.
set(TAUT_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined")

if(TAUT_SANITIZE)
    add_compile_options(
        -fsanitize=${TAUT_SANITIZE}
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all   # abort on first UBSan hit; no silent recovery
    )
    add_link_options(-fsanitize=${TAUT_SANITIZE})
    message(STATUS "taut: sanitizers enabled -> ${TAUT_SANITIZE}")
endif()
