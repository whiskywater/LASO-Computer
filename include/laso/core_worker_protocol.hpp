#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace laso::core_worker_protocol {

using Json = nlohmann::json;
inline constexpr std::uint32_t version = 1;
inline constexpr std::size_t max_frame_bytes = 1024 * 1024;
inline constexpr std::size_t max_id_bytes = 512;
inline constexpr std::size_t max_error_bytes = 512;
inline constexpr std::size_t max_metadata_bytes = 64 * 1024;
inline constexpr std::size_t max_artifact_references = 16;

struct Request {
    std::string request_id;
    std::string operation;
    std::string job_id;
    std::string external_job_id;
    Json payload = Json::object();
};

struct Response {
    std::string request_id;
    bool ok{true};
    std::string state{"Failed"};
    std::string external_job_id;
    Json payload = Json::object();
    Json metadata = Json::object();
    Json continuation = nullptr;
    Json artifacts = Json::array();
    Json usage = Json::object();
    std::string error;
};

inline Request parse_request_frame(std::string_view frame) {
    if (frame.empty() || frame.size() > max_frame_bytes + 1 || frame.back() != '\n')
        throw std::invalid_argument("invalid worker frame boundary");
    frame.remove_suffix(1);
    if (frame.size() > max_frame_bytes)
        throw std::invalid_argument("worker frame exceeds size limit");
    if (!frame.empty() && frame.back() == '\r') frame.remove_suffix(1);
    if (frame.empty() || frame.find_first_of("\r\n") != std::string_view::npos)
        throw std::invalid_argument("invalid worker frame boundary");

    bool duplicate_key = false;
    std::vector<std::set<std::string>> keys_by_depth;
    const auto callback = [&duplicate_key, &keys_by_depth](int depth, Json::parse_event_t event, Json& value) {
        if (depth < 0) return true;
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (keys_by_depth.size() <= index) keys_by_depth.resize(index + 1);
            keys_by_depth[index].clear();
        } else if (event == Json::parse_event_t::key) {
            if (keys_by_depth.size() <= index) keys_by_depth.resize(index + 1);
            if (!keys_by_depth[index].insert(value.get<std::string>()).second) duplicate_key = true;
        }
        return true;
    };

    Json message;
    try {
        message = Json::parse(frame.begin(), frame.end(), callback, true, false);
    } catch (...) {
        throw std::invalid_argument("malformed worker JSON");
    }
    if (duplicate_key || !message.is_object()) throw std::invalid_argument("invalid worker message");
    const auto required_string = [&message](const char* name) -> const std::string& {
        if (!message.contains(name) || !message.at(name).is_string() || message.at(name).get_ref<const std::string&>().empty() ||
            message.at(name).get_ref<const std::string&>().size() > max_id_bytes)
            throw std::invalid_argument("worker message is missing a required identifier");
        return message.at(name).get_ref<const std::string&>();
    };
    if (!message.contains("protocol_version") ||
        !(message.at("protocol_version").is_number_unsigned() || message.at("protocol_version").is_number_integer()) ||
        message.at("protocol_version").get<std::int64_t>() != version)
        throw std::invalid_argument("incompatible worker protocol version");

    Request request;
    request.request_id = required_string("request_id");
    request.operation = required_string("operation");
    static const std::set<std::string> operations{"hello", "submit", "status", "result", "cancel", "shutdown"};
    if (!operations.contains(request.operation)) throw std::invalid_argument("invalid worker operation");
    if (!message.contains("job_id") || !message.at("job_id").is_string() ||
        !message.contains("external_job_id") || !message.at("external_job_id").is_string())
        throw std::invalid_argument("invalid worker identifiers");
    request.job_id = message.at("job_id").get<std::string>();
    request.external_job_id = message.at("external_job_id").get<std::string>();
    if (request.job_id.size() > max_id_bytes || request.external_job_id.size() > max_id_bytes)
        throw std::invalid_argument("worker identifier exceeds size limit");
    // Core assigns job IDs to submitted work. Lifecycle/polling operations use
    // external_job_id where needed and leave job_id empty.
    if (request.operation == "submit" && request.job_id.empty())
        throw std::invalid_argument("worker job identifier is required");
    if ((request.operation == "status" || request.operation == "result" || request.operation == "cancel") &&
        request.external_job_id.empty())
        throw std::invalid_argument("external worker job identifier is required");
    if (!message.contains("payload") || !message.at("payload").is_object())
        throw std::invalid_argument("worker payload must be an object");
    request.payload = message.at("payload");
    return request;
}

inline std::string serialize_response_frame(const Response& response) {
    if (response.request_id.empty() || response.request_id.size() > max_id_bytes || response.state.empty() ||
        response.state.size() > 32 || response.external_job_id.size() > max_id_bytes ||
        !response.metadata.is_object() || !response.artifacts.is_array() || !response.usage.is_object() ||
        response.metadata.dump().size() > max_metadata_bytes || response.artifacts.size() > max_artifact_references ||
        response.error.size() > max_error_bytes ||
        (!response.ok && response.error.empty()))
        throw std::invalid_argument("invalid worker response");
    Json message{{"protocol_version", version}, {"request_id", response.request_id}, {"ok", response.ok},
                 {"state", response.state}, {"external_job_id", response.external_job_id},
                 {"payload", response.payload}, {"metadata", response.metadata},
                 {"continuation", response.continuation}, {"artifacts", response.artifacts},
                 {"usage", response.usage}, {"error", response.error}};
    auto frame = message.dump(-1, ' ', false, Json::error_handler_t::strict);
    if (frame.size() > max_frame_bytes) throw std::length_error("worker response exceeds frame limit");
    frame.push_back('\n');
    return frame;
}

} // namespace laso::core_worker_protocol
