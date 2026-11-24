#include "lb/registry.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace lb {
namespace {

ServerListSnapshot makeEligibleSnapshot(const ServerList& backends, const BackendPtr& changed = {},
                                        BackendState newState = BackendState::UNKNOWN) {
    auto eligible = std::make_shared<ServerList>();
    eligible->reserve(backends.size());
    for (const auto& backend : backends) {
        const auto state = backend == changed ? newState : backend->state();
        if (state == BackendState::HEALTHY) {
            eligible->push_back(backend);
        }
    }
    return eligible;
}

void validateConfig(const std::vector<BackendConfig>& backends) {
    std::unordered_set<std::string> ids;
    for (const auto& backend : backends) {
        if (backend.id.empty() || backend.host.empty() || backend.port == 0) {
            throw std::invalid_argument("Backend requires a nonempty id/host and nonzero port");
        }
        if (!ids.insert(backend.id).second) {
            throw std::invalid_argument("Duplicate backend id: " + backend.id);
        }
    }
}

void validateState(BackendState state) {
    switch (state) {
    case BackendState::UNKNOWN:
    case BackendState::HEALTHY:
    case BackendState::UNHEALTHY:
    case BackendState::DRAINING:
        return;
    }
    throw std::invalid_argument("Invalid backend state");
}

} // namespace

BackendRegistry::BackendRegistry(const std::vector<BackendConfig>& backends)
    : m_eligible(std::make_shared<const ServerList>()) {
    replaceBackends(backends);
}

ServerListSnapshot BackendRegistry::snapshot() const { return m_eligible.load(); }

ServerList BackendRegistry::configuredBackends() const {
    const std::lock_guard lock(m_writerMutex);
    return m_backends;
}

BackendPtr BackendRegistry::findBackend(std::string_view id) const {
    const std::lock_guard lock(m_writerMutex);
    const auto found = std::find_if(m_backends.begin(), m_backends.end(),
                                    [id](const auto& backend) { return backend->id == id; });
    return found == m_backends.end() ? nullptr : *found;
}

void BackendRegistry::replaceBackends(const std::vector<BackendConfig>& backends) {
    validateConfig(backends);
    const std::lock_guard lock(m_writerMutex);
    ServerList replacement;
    replacement.reserve(backends.size());
    for (const auto& config : backends) {
        const auto found =
            std::find_if(m_backends.begin(), m_backends.end(), [&config](const auto& backend) {
                return backend->id == config.id && backend->host == config.host &&
                       backend->port == config.port;
            });
        replacement.push_back(found == m_backends.end() ? std::make_shared<BackendServer>(
                                                              config.id, config.host, config.port)
                                                        : *found);
    }
    // Complete allocations before mutating published membership.
    auto eligible = makeEligibleSnapshot(replacement);
    m_backends = std::move(replacement);
    m_eligible.store(std::move(eligible));
}

bool BackendRegistry::setState(std::string_view id, BackendState state) {
    validateState(state);
    const std::lock_guard lock(m_writerMutex);
    const auto found = std::find_if(m_backends.begin(), m_backends.end(),
                                    [id](const auto& backend) { return backend->id == id; });
    if (found == m_backends.end()) {
        return false;
    }
    publishState(*found, state);
    return true;
}

bool BackendRegistry::setStateIfCurrent(const BackendPtr& backend, BackendState expected,
                                        BackendState state) {
    validateState(state);
    const std::lock_guard lock(m_writerMutex);
    if (!backend || backend->state() != expected || expected == BackendState::DRAINING ||
        std::find(m_backends.begin(), m_backends.end(), backend) == m_backends.end()) {
        return false;
    }
    publishState(backend, state);
    return true;
}

void BackendRegistry::publishState(const BackendPtr& backend, BackendState state) {
    if (backend->state() == state) {
        return;
    }
    // Allocation failure must not change health state without publishing it.
    auto eligible = makeEligibleSnapshot(m_backends, backend, state);
    backend->m_state.store(state, std::memory_order_release);
    backend->stateChanges[static_cast<std::size_t>(state)].fetch_add(1, std::memory_order_relaxed);
    m_eligible.store(std::move(eligible));
}

} // namespace lb
