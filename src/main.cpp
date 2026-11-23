#include "lb/foundation.hpp"
#include "lb/config.hpp"
#include "lb/listener.hpp"

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
        const auto options = lb::parseProxyOptions(args);
        if (options.help) {
            std::cout << "Usage: load_balancer [--listen 127.0.0.1] [--port 8080]\n"
                         "  [--backend ID,HOST,PORT]... [--upstream-timeout-ms 3000]\n"
                         "  [--connect-timeout-ms 1000] [--client-timeout-ms 10000]\n"
                         "  [--max-body-bytes 1048576] [--max-header-bytes 16384]\n"
                         "  [--smoke] [--help]\n"
                         "Port zero chooses a free port. Startup probes GET /health once.\n";
            return 0;
        }
        if (!options.smoke) {
            lb::runProxy(options);
            return 0;
        }
        const auto result = lb::runFoundationSmoke();
        if (!result.timerCompleted) {
            std::cerr << "Foundation timer did not complete\n";
            return 1;
        }
        std::cout << "Phase 0 foundation: Asio coroutine timer completed; Beast parsed "
                  << result.method << ' ' << result.target << " (body: " << result.body << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "load_balancer: " << error.what() << '\n';
        return 1;
    }
}
