#include "lb/test_backend.hpp"

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string_view> args;
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        const auto options = lb::simulator::parseOptions(args);
        if (options.help) {
            std::cout << "Usage: test_backend [--id A] [--listen 127.0.0.1] [--port 9000]\n"
                         "                    [--not-ready] [--test-mode] [--help]\n"
                         "--port 0 chooses an available port; --test-mode requires loopback.\n"
                         "Test mode enables POST /admin/ready?value=true|false.\n";
            return 0;
        }
        lb::simulator::run(options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_backend: " << error.what() << '\n';
        return 1;
    }
}
