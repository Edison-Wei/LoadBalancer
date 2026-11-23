#include "lb/foundation.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        const auto result = lb::runFoundationSmoke();
        if (!result.timerCompleted) {
            std::cerr << "Foundation timer did not complete\n";
            return 1;
        }
        std::cout << "Phase 0 foundation: Asio coroutine timer completed; Beast parsed "
                  << result.method << ' ' << result.target << " (body: " << result.body << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Foundation smoke failed: " << error.what() << '\n';
        return 1;
    }
}
