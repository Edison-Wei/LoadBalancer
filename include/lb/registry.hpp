#pragma once

#include "lb/atomic_shared_ptr.hpp"
#include "lb/backend.hpp"

#include <mutex>
#include <string_view>
#include <vector>

namespace lb {

/// Publishes immutable eligible membership in configuration order. UNKNOWN,
/// UNHEALTHY, and DRAINING are excluded. One writer mutex covers membership,
/// state changes, and publication, preventing lost read/modify/write updates.
/// Readers release the snapshot publication lock before selecting or doing I/O.
class BackendRegistry {
public:
    explicit BackendRegistry(const std::vector<BackendConfig>& backends = {});

    [[nodiscard]] ServerListSnapshot snapshot() const;
    [[nodiscard]] BackendPtr findBackend(std::string_view id) const;

    /// Reuses state/counters for unchanged (id, host, port) identities. New or
    /// changed endpoints start UNKNOWN. Removed objects outlive outstanding readers.
    /// Rejects invalid/duplicate IDs before altering the registry.
    void replaceBackends(const std::vector<BackendConfig>& backends);

    /// Returns false for an unknown ID; otherwise publishes the new eligible list.
    /// A held old snapshot may still select this backend after this call returns.
    bool setState(std::string_view id, BackendState state);

private:
    mutable std::mutex m_writerMutex;
    ServerList m_backends;
    AtomicSharedPtr<const ServerList> m_eligible;
};

} // namespace lb
