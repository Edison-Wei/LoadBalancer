#include "lb/atomic_shared_ptr.hpp"

#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void checkLifetime() {
    lb::AtomicSharedPtr<const int> published;
    require(!published.load(), "Default snapshot must be empty");
    published.store(std::make_shared<const int>(42));
    auto held = published.load();
    const std::weak_ptr<const int> old = held;
    published.store(std::make_shared<const int>(43));
    require(*held == 42 && *published.load() == 43, "Replacement changed held snapshot");
    held.reset();
    require(old.expired(), "Old snapshot leaked after its last reader released it");
    published.store(nullptr);
    require(!published.load(), "Clearing publication failed");

    // Reenter the wrapper from a deleter: destruction inside the mutex deadlocks.
    std::atomic<bool> deleted = false;
    auto object = std::make_unique<const int>(7);
    published.store(std::shared_ptr<const int>(object.release(), [&](const int* value) {
        deleted.store(!published.load());
        delete value;
    }));
    published.store(nullptr);
    require(deleted.load(), "Deleter could not reenter load after clearing publication");
}

struct Snapshot {
    int generation;
    std::vector<int> entries;
};

void checkConcurrentPublication() {
    constexpr int READERS = 4;
    constexpr int ITERATIONS = 4000;
    lb::AtomicSharedPtr<const Snapshot> published(
        std::make_shared<const Snapshot>(Snapshot{0, std::vector<int>(16, 0)}));
    std::atomic<int> ready = 0;
    std::atomic<bool> start = false;
    std::atomic<bool> consistent = true;
    std::vector<std::jthread> readers;
    for (int reader = 0; reader < READERS; ++reader) {
        readers.emplace_back([&] {
            ready.fetch_add(1);
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
                const auto observed = published.load();
                if (!observed || observed->entries.size() != 16) {
                    consistent.store(false);
                    continue;
                }
                for (const auto value : observed->entries) {
                    if (value != observed->generation) {
                        consistent.store(false);
                    }
                }
            }
        });
    }
    while (ready.load() != READERS) {
        std::this_thread::yield();
    }
    start.store(true);
    for (int generation = 1; generation <= ITERATIONS; ++generation) {
        published.store(std::make_shared<const Snapshot>(
            Snapshot{generation, std::vector<int>(16, generation)}));
    }
    for (auto& reader : readers) {
        reader.join();
    }
    require(consistent.load(), "Reader observed inconsistent snapshot data");
    require(published.load()->generation == ITERATIONS, "Final publication was lost");
}

} // namespace

int main() {
    try {
        checkLifetime();
        checkConcurrentPublication();
        std::cout << "Snapshot lifetime, reentrant deletion, and concurrent publication passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
