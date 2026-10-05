#include "laso/core_worker_adapter.hpp"

#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <set>
#include <thread>
#include <Windows.h>

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

std::string utc_timestamp(std::chrono::system_clock::time_point point) {
    const auto time = std::chrono::system_clock::to_time_t(point);
    std::tm value{};
    gmtime_s(&value, &time);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool read_timed_line(std::istream& input, std::string& line, std::size_t limit,
                     std::chrono::steady_clock::time_point deadline,
                     const std::function<bool()>& cancelled) {
    for (;;) {
        if (cancelled && cancelled()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (input.rdbuf()->in_avail() <= 0) {
            if (input.eof()) return false;
            if (&input == &std::cin) {
                DWORD available = 0;
                const auto handle = GetStdHandle(STD_INPUT_HANDLE);
                if (handle == INVALID_HANDLE_VALUE || handle == nullptr ||
                    !PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return false;
                if (available == 0) { Sleep(5); continue; }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }
        char ch = 0;
        if (!input.get(ch)) return false;
        if (ch == '\n') return true;
        if (line.size() >= limit) return false;
        line.push_back(ch);
    }
}

Json parse_unique_json(const std::string& text) {
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&duplicate, &keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth < 0) return true;
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (keys.size() <= index) keys.resize(index + 1);
            keys[index].clear();
        } else if (event == Json::parse_event_t::key) {
            if (keys.size() <= index) keys.resize(index + 1);
            if (!keys[index].insert(value.get<std::string>()).second) duplicate = true;
        }
        return true;
    };
    auto value = Json::parse(text, callback, true, false);
    if (duplicate || !value.is_object()) throw std::invalid_argument("invalid interaction response");
    return value;
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
        std::string worker_id = "laso-computer";
        if (request.payload.contains("worker_id")) {
            if (!request.payload.at("worker_id").is_string() || request.payload.at("worker_id").get_ref<const std::string&>().size() > 512)
                return adapter_error(request.request_id, "invalid worker identity");
            worker_id = request.payload.at("worker_id").get<std::string>();
            if (worker_id.empty()) worker_id = "laso-computer";
        }
        legacy["worker_id"] = worker_id;
        if (request.payload.contains("durable_session_id")) {
            if (!request.payload.at("durable_session_id").is_string() ||
                request.payload.at("durable_session_id").get_ref<const std::string&>().size() > 512)
                return adapter_error(request.request_id, "invalid session identity");
            legacy["session_id"] = request.payload.at("durable_session_id");
        }
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
    interaction_input_ = &input;
    interaction_output_ = &output;
    interaction_transport_failed_.store(false);
    stopping_.store(false);
    dispatcher_.set_policy_interaction_handler([this](const PolicyInteraction& interaction) {
        if (!interaction_input_ || !interaction_output_)
            return PolicyInteractionResult{"cancelled", Json::object(), "interaction unavailable"};
        return exchange_interaction(interaction, *interaction_input_, *interaction_output_);
    });
    struct Cleanup {
        CoreWorkerAdapter& adapter;
        ~Cleanup() {
            adapter.stopping_.store(true);
            adapter.dispatcher_.set_policy_interaction_handler({});
            adapter.clear_interactions();
            adapter.interaction_input_ = nullptr;
            adapter.interaction_output_ = nullptr;
        }
    } cleanup{*this};
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
        if (interaction_transport_failed_.load()) {
            diagnostics << "worker interaction channel failed\n";
            return 2;
        }
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

PolicyInteractionResult CoreWorkerAdapter::exchange_interaction(const PolicyInteraction& interaction,
                                                                std::istream& input, std::ostream& output) {
    const auto failed = [] { return PolicyInteractionResult{"cancelled", Json::object(), "interaction unavailable"}; };
    std::unique_lock io_lock(interaction_io_mutex_);
    if (stopping_.load() || interaction.worker_id.empty() ||
        interaction.worker_id.size() > 512 || interaction.worker_job_id.empty() ||
        interaction.worker_job_id.size() > 512 || interaction.external_job_id.empty() ||
        interaction.external_job_id.size() > 512 || interaction.session_id.size() > 512 ||
        interaction.title.size() > 4096 || interaction.summary.size() > 4096 ||
        !interaction.payload.is_object() || interaction.payload.dump().size() > 64 * 1024 ||
        (interaction.type != "approval" && interaction.type != "permission" && interaction.type != "question") ||
        interaction.risk.size() > 128 || interaction.category.size() > 128) {
        interaction_transport_failed_.store(true);
        return failed();
    }
    if (interaction.cancelled && interaction.cancelled()) return {"cancelled", Json::object(), "interaction cancelled"};
    const auto opaque_id = interaction_id_factory_ ? interaction_id_factory_() : random_id();
    const auto request_id = "interaction-" + opaque_id;
    if (opaque_id.empty() || request_id.size() > 512) { interaction_transport_failed_.store(true); return failed(); }
    {
        std::scoped_lock lock(pending_mutex_);
        if (pending_.size() >= 64 || pending_.contains(request_id)) {
            interaction_transport_failed_.store(true);
            return failed();
        }
        pending_.emplace(request_id, interaction.type);
    }
    struct PendingCleanup {
        CoreWorkerAdapter& adapter;
        std::string id;
        ~PendingCleanup() { std::scoped_lock lock(adapter.pending_mutex_); adapter.pending_.erase(id); }
    } pending_cleanup{*this, request_id};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interaction_timeout_ms_);
    const auto wall_deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(interaction_timeout_ms_);
    const auto now = std::chrono::system_clock::now();
    Json message{{"protocol_version", core_worker_protocol::version}, {"message_type", "worker_request"},
                 {"request_id", request_id}, {"worker_job_id", interaction.worker_job_id},
                 {"worker_id", interaction.worker_id}, {"external_job_id", interaction.external_job_id},
                 {"session_id", interaction.session_id}, {"request_type", interaction.type},
                 {"title", interaction.title}, {"summary", interaction.summary},
                 {"payload", interaction.payload},
                 {"created_at", utc_timestamp(now)}, {"deadline", utc_timestamp(wall_deadline)},
                 {"risk", interaction.risk}, {"category", interaction.category}};
    auto encoded = message.dump(-1, ' ', false, Json::error_handler_t::strict);
    if (encoded.size() > core_worker_protocol::max_frame_bytes) {
        interaction_transport_failed_.store(true);
        return failed();
    }
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.put('\n');
    output.flush();
    if (!output.good()) { interaction_transport_failed_.store(true); return failed(); }

    std::string line;
    if (!read_timed_line(input, line, core_worker_protocol::max_frame_bytes, deadline,
                         interaction.cancelled)) {
        if (!(interaction.cancelled && interaction.cancelled())) interaction_transport_failed_.store(true);
        return failed();
    }
    Json answer;
    try { answer = parse_unique_json(line); } catch (...) { interaction_transport_failed_.store(true); return failed(); }
    static const std::set<std::string> allowed_fields{
        "protocol_version", "message_type", "request_id", "decision", "payload", "reason"};
    for (auto it = answer.begin(); it != answer.end(); ++it)
        if (!allowed_fields.contains(it.key())) { interaction_transport_failed_.store(true); return failed(); }
    if (!answer.contains("protocol_version") || !answer["protocol_version"].is_number_integer() ||
        answer["protocol_version"] != core_worker_protocol::version ||
        !answer.contains("message_type") || !answer["message_type"].is_string() ||
        answer["message_type"] != "worker_response" ||
        !answer.contains("request_id") || !answer["request_id"].is_string() ||
        answer["request_id"] != request_id ||
        !answer.contains("decision") || !answer["decision"].is_string() ||
        !answer.contains("payload") || !answer["payload"].is_object() || answer["payload"].dump().size() > 64 * 1024 ||
        !answer.contains("reason") || !answer["reason"].is_string() || answer["reason"].get_ref<const std::string&>().size() > 4096) {
        interaction_transport_failed_.store(true);
        return failed();
    }
    const auto decision = answer["decision"].get<std::string>();
    const bool terminal = decision == "cancelled" || decision == "expired";
    const bool compatible = interaction.type == "question"
                                ? decision == "answered" || terminal
                                : decision == "approved" || decision == "denied" || terminal;
    {
        std::scoped_lock lock(pending_mutex_);
        const auto found = pending_.find(request_id);
        if (found == pending_.end() || found->second != interaction.type || !compatible) {
            interaction_transport_failed_.store(true);
            return failed();
        }
    }
    return {decision, answer["payload"], answer["reason"]};
}

void CoreWorkerAdapter::clear_interactions() noexcept {
    std::scoped_lock lock(pending_mutex_);
    pending_.clear();
}

} // namespace laso
