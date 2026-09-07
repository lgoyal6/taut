#include <iostream>
#include <string_view>

#include <taut/version.h>

int main() {
    if (std::string_view(taut::version()) != TAUT_EXPECTED_VERSION) {
        std::cerr << "package metadata and linked library version disagree\n";
        return 1;
    }
    std::cout << "taut " << taut::version() << " consumer ok\n";
}
