#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace lb {

/// Atomic publication of shared ownership, with an Apple-compatible mutex fallback.
/// Only load/store are provided, not the full std::atomic API. Neither implementation
/// promises lock freedom. Publish immutable objects and serialize read/modify/write
/// updates separately; individually safe loads/stores do not prevent lost updates.
/// The wrapper must outlive all concurrent calls, just like a mutex or std::atomic.
template <typename T> class AtomicSharedPtr {
  public:
    AtomicSharedPtr() = default;
    explicit AtomicSharedPtr(std::shared_ptr<T> value) : m_value(std::move(value)) {}

    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    /// Takes an owning snapshot; no lock remains held when the caller uses it.
    [[nodiscard]] std::shared_ptr<T> load() const {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L &&              \
    !defined(LB_FORCE_MUTEX_SHARED_PTR)
        return m_value.load(std::memory_order_acquire);
#else
        const std::lock_guard lock(m_mutex);
        return m_value;
#endif
    }

    /// Publishes the fully initialized value. Releases the previous object outside
    /// the fallback mutex so destructors/custom deleters may safely reenter load().
    void store(std::shared_ptr<T> value) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L &&              \
    !defined(LB_FORCE_MUTEX_SHARED_PTR)
        m_value.store(std::move(value), std::memory_order_release);
#else
        {
            const std::lock_guard lock(m_mutex);
            m_value.swap(value);
        }
#endif
    }

  private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L &&              \
    !defined(LB_FORCE_MUTEX_SHARED_PTR)
    std::atomic<std::shared_ptr<T>> m_value;
#else
    mutable std::mutex m_mutex;
    std::shared_ptr<T> m_value;
#endif
};

} // namespace lb
