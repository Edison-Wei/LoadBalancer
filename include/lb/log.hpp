#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace lb {
/// Buffer each event before a short output lock; worker logs cannot interleave.
/// Uses standard streams/mutex because this Apple library lacks osyncstream.
class LogLine : public std::ostringstream {
  public:
    ~LogLine() noexcept {
        try {
            auto text = str();
            if (text.empty() || text.back() != '\n') {
                text += '\n';
            }
            const std::lock_guard lock(outputMutex());
            std::clog << text;
            std::clog.flush();
        } catch (...) {
            // Logging must not throw while another operation is unwinding.
        }
    }

  private:
    static std::mutex& outputMutex() {
        static std::mutex mutex;
        return mutex;
    }
};
} // namespace lb
