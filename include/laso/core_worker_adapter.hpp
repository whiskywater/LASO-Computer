#pragma once

#include "laso/core_worker_protocol.hpp"
#include "laso/protocol.hpp"

#include <istream>
#include <atomic>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace laso {

// Adapts current Core process-worker envelopes to the endpoint's existing
// policy-aware WorkerProtocol dispatcher. Correlated approval, permission,
// and question exchanges share the active bounded request channel.
class CoreWorkerAdapter {
public:
    explicit CoreWorkerAdapter(WorkerProtocol& dispatcher, unsigned interaction_timeout_ms = 60000,
                               std::function<std::string()> interaction_id_factory = {})
        : dispatcher_(dispatcher), interaction_timeout_ms_(interaction_timeout_ms == 0 ? 1U :
                                                              interaction_timeout_ms > 300000U ? 300000U :
                                                                                                  interaction_timeout_ms),
          interaction_id_factory_(std::move(interaction_id_factory)) {}

    [[nodiscard]] nlohmann::json handle(const core_worker_protocol::Request& request);
    int serve(std::istream& input, std::ostream& output, std::ostream& diagnostics);

    // Performs one correlated interaction exchange. Call only while Core is
    // waiting for the response to an active worker operation.
    PolicyInteractionResult exchange_interaction(const PolicyInteraction& interaction,
                                                 std::istream& input, std::ostream& output);

private:
    void clear_interactions() noexcept;

    WorkerProtocol& dispatcher_;
    unsigned interaction_timeout_ms_;
    std::function<std::string()> interaction_id_factory_;
    std::mutex interaction_io_mutex_;
    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::string> pending_;
    std::atomic_bool interaction_transport_failed_{false};
    std::atomic_bool stopping_{false};
    std::istream* interaction_input_{nullptr};
    std::ostream* interaction_output_{nullptr};
};

} // namespace laso
