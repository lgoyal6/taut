// Build/link/parse sanity check for the Week 1 S1 scaffold. Replaced by real per-module
// unit tests in S2. It also #includes the public headers so the API sketch is forced
// through the compiler (headers alone are not otherwise compiled).
#include <cstdio>
#include <cstring>

#include "taut/config.h"
#include "taut/node.h"
#include "taut/types.h"
#include "taut/version.h"

int main() {
    taut::Config cfg;
    if (cfg.window_pkts != 64) {
        return 1;
    }
    if (cfg.rto_floor.count() != 25) {
        return 1;
    }
    if (std::strlen(taut::version()) == 0) {
        return 1;
    }
    (void)taut::Class::ReliableUnordered;
    (void)taut::MemberState::Alive;
    std::puts("taut smoke ok");
    return 0;
}
