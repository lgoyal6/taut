#include "taut/version.h"

#ifndef TAUT_VERSION_STRING
// Kept so the file still compiles outside the project's own build (a consumer
// vendoring this .cc directly, for instance); the CMake build always defines it.
#define TAUT_VERSION_STRING "unknown"
#endif

namespace taut {

const char* version() {
    return TAUT_VERSION_STRING;
}

} // namespace taut
