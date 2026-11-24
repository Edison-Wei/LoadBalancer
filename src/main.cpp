#include "lb/config.hpp"
#include "lb/foundation.hpp"
#include "lb/listener.hpp"
#include "lb/version.hpp"

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
        if (options.version) {
            std::cout << lb::BUILD_INFO_JSON << '\n';
            return 0;
        }
        if (options.help) {
            std::cout
                << "Usage: load_balancer [--listen 127.0.0.1] [--port 8080]\n"
                   "  [--metrics-listen 127.0.0.1] [--metrics-port 9091]\n"
                   "  [--backend ID,HOST,PORT]... [--upstream-timeout-ms 3000]\n"
                   "  [--connect-timeout-ms 1000] [--client-timeout-ms 10000]\n"
                   "  [--upstream-write-timeout-ms 3000] [--upstream-read-timeout-ms 3000]\n"
                   "  [--client-read-timeout-ms 10000] [--client-write-timeout-ms 10000]\n"
                   "  [--workers 4] [--shutdown-grace-ms 5000]\n"
                   "  [--max-connections 4096] [--max-inflight 256] [--max-upstream-connections "
                   "256]\n"
                   "  [--max-body-bytes 1048576] [--max-header-bytes 16384]\n"
                   "  [--health-path /health] [--health-interval-ms 2000]\n"
                   "  [--health-connect-timeout-ms 1000] [--health-response-timeout-ms 2000]\n"
                   "  [--health-failure-threshold 3] [--health-success-threshold 2]\n"
                   "  [--smoke] [--help] [--version]\n"
                   "Port zero chooses a free port. Readiness probes recur automatically.\n";
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
        std::cout << "Foundation smoke: Asio coroutine timer completed; Beast parsed "
                  << result.method << ' ' << result.target << " (body: " << result.body << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "load_balancer: " << error.what() << '\n';
        return 1;
    }
}
