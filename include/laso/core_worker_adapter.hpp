#pragma once

#include "laso/core_worker_protocol.hpp"
#include "laso/protocol.hpp"

#include <istream>
#include <ostream>

namespace laso {

// Adapts current Core process-worker envelopes to the endpoint's existing
// policy-aware WorkerProtocol dispatcher. Interaction messages are not yet
// supported; require_approval remains fail-closed in WorkerProtocol.
class CoreWorkerAdapter {
public:
    explicit CoreWorkerAdapter(WorkerProtocol& dispatcher) : dispatcher_(dispatcher) {}

    [[nodiscard]] nlohmann::json handle(const core_worker_protocol::Request& request);
    int serve(std::istream& input, std::ostream& output, std::ostream& diagnostics);

private:
    WorkerProtocol& dispatcher_;
};

} // namespace laso
