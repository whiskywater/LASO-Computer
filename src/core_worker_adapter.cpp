#include "laso/core_worker_adapter.hpp"

#include <string>
#include <vector>

namespace laso {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_metadata_bytes = 64 * 1024;
constexpr std::size_t max_artifact_references = 16;
constexpr std::size_t max_error_bytes = 512;

Json adapter_error(const std::string& request_id, const std::string& message) {
    return {{"protocol_version", core_worker_protocol::version}, {"request_id", request_id},
            {"ok", false}, {"state", "Failed"}, {"external_job_id", ""},
            {"metadata", Json::object()}, {"artifacts", Json::array()},
            {"usage", Json::object()}, {"continuation", nullptr}, {"error", message}};
}

bool bounded_error(const Json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() <= max_error_bytes;
}

} // namespace

Json CoreWorkerAdapter::handle(const core_worker_protocol::Request& request) {
    Json legacy{{"protocol_version", core_worker_protocol::version},
                {"request_id", request.request_id}, {"operation", request.operation}};
    if (!request.job_id.empty()) legacy["job_id"] = request.job_id;
    if (!request.external_job_id.empty()) legacy["external_job_id"] = request.external_job_id;

    if (request.operation == "hello") {
        legacy["payload"] = {{"client", "laso"}};
    } else if (request.operation == "submit") {
        if (request.job_id.empty() || !request.payload.is_object() ||
            !request.payload.contains("capability") || !request.payload.at("capability").is_string() ||
            request.payload.at("capability").get_ref<const std::string&>().empty() ||
            request.payload.at("capability").get_ref<const std::string&>().size() > 128 ||
            !request.payload.contains("input") || !request.payload.at("input").is_object())
            return adapter_error(request.request_id, "invalid capability request");
        legacy["payload"] = {{"capability", request.payload.at("capability")},
                             {"arguments", request.payload.at("input")}};
    } else {
        legacy["payload"] = Json::object();
    }

    Json local;
    try {
        local = dispatcher_.handle(legacy);
    } catch (...) {
        return adapter_error(request.request_id, "endpoint request failed");
    }
    if (!local.is_object()) return adapter_error(request.request_id, "endpoint response is invalid");

    const bool ok = local.value("ok", false);
    const auto state = local.value("state", std::string("Failed"));
    if (state.empty()) return adapter_error(request.request_id, "endpoint response is invalid");

    Json response{{"protocol_version", core_worker_protocol::version},
                  {"request_id", request.request_id}, {"ok", ok}, {"state", state},
                  {"external_job_id", local.value("external_job_id", request.external_job_id)},
                  {"metadata", Json::object()}, {"artifacts", Json::array()},
                  {"usage", Json::object()}, {"continuation", nullptr}, {"error", ""}};

    if (request.operation == "hello" && ok) {
        std::vector<std::string> capabilities;
        const auto items = local.value("payload", Json::object()).value("capabilities", Json::array());
        if (!items.is_array()) return adapter_error(request.request_id, "endpoint capabilities are invalid");
        for (const auto& item : items) {
            if (item.is_object() && item.value("available", false) && item.contains("name") && item.at("name").is_string())
                capabilities.push_back(item.at("name").get<std::string>());
        }
        response["metadata"] = {{"id", "laso-computer"}, {"name", "LASO Computer"},
                                {"version", "1"}, {"description", "Local Windows capability worker"},
                                {"plugin", "native"}, {"event_schema", ""},
                                {"event_source_id", "worker.laso-computer"}, {"status", "healthy"},
                                {"capabilities", capabilities}, {"local", true}, {"remote", false},
                                {"healthy", true}, {"enabled", true}, {"supports_status", true},
                                {"supports_recovery", false}, {"supports_cancellation", true}};
    } else {
        if (local.contains("payload")) response["payload"] = local.at("payload");
        if (local.contains("metadata") && local.at("metadata").is_object()) response["metadata"] = local.at("metadata");
        if (local.contains("artifacts") && local.at("artifacts").is_array()) response["artifacts"] = local.at("artifacts");
        if (local.contains("usage") && local.at("usage").is_object()) response["usage"] = local.at("usage");
        if (local.contains("continuation")) response["continuation"] = local.at("continuation");
        if (local.contains("error") && bounded_error(local.at("error"))) response["error"] = local.at("error");
        if (request.operation == "cancel" && ok) {
            const auto acknowledged = local.value("payload", Json::object()).value("cancellation_requested", false);
            response["acknowledged"] = acknowledged;
            response["payload"] = {{"acknowledged", acknowledged}};
        }
    }

    if (!response["metadata"].is_object() || response["metadata"].dump().size() > max_metadata_bytes ||
        !response["artifacts"].is_array() || response["artifacts"].size() > max_artifact_references ||
        !response["usage"].is_object() || !bounded_error(response["error"]))
        return adapter_error(request.request_id, "endpoint response exceeds protocol limits");
    return response;
}

int CoreWorkerAdapter::serve(std::istream& input, std::ostream& output, std::ostream& diagnostics) {
    for (;;) {
        std::string frame;
        bool too_large = false;
        bool read_any = false;
        bool terminated = false;
        char ch = 0;
        while (input.get(ch)) {
            read_any = true;
            if (ch == '\n') { terminated = true; break; }
            if (frame.size() <= core_worker_protocol::max_frame_bytes)
                frame.push_back(ch);
            else
                too_large = true;
        }
        if (!read_any && input.eof()) return 0;
        if (too_large || !terminated) {
            diagnostics << "invalid worker frame\n";
            return 2;
        }
        frame.push_back('\n');
        core_worker_protocol::Request request;
        try {
            request = core_worker_protocol::parse_request_frame(frame);
        } catch (...) {
            diagnostics << "invalid worker request\n";
            return 2;
        }
        const bool shutdown = request.operation == "shutdown";
        const auto response = handle(request);
        auto encoded = response.dump(-1, ' ', false, Json::error_handler_t::strict);
        if (encoded.size() > core_worker_protocol::max_frame_bytes) {
            diagnostics << "worker response exceeds protocol limit\n";
            return 2;
        }
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        output.put('\n');
        output.flush();
        if (!output.good()) return 2;
        if (shutdown) return 0;
    }
}

} // namespace laso
