#include "fem/core/ipc/json_rpc.h"

#include <charconv>
#include <exception>
#include <utility>

namespace fem::ipc {

namespace {

/// Longest header block accepted before its empty line.
constexpr std::size_t max_header = 4096;

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// The payload length named by a header block (without its final empty line).
std::size_t content_length(std::string_view headers) {
    std::optional<std::size_t> length;
    while (!headers.empty()) {
        const std::size_t end = headers.find("\r\n");
        const std::string_view line = headers.substr(0, end);
        headers.remove_prefix(end == std::string_view::npos ? headers.size() : end + 2);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw ProtocolError("malformed header line '" + std::string(line) + "'");
        }
        if (!equals_ignoring_case(trim(line.substr(0, colon)), "content-length")) {
            continue;
        }
        const std::string_view value = trim(line.substr(colon + 1));
        std::size_t parsed = 0;
        const auto [rest, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc() || rest != value.data() + value.size() || value.empty()) {
            throw ProtocolError("malformed Content-Length '" + std::string(value) + "'");
        }
        length = parsed;
    }
    if (!length) {
        throw ProtocolError("a frame without Content-Length");
    }
    if (*length > FrameReader::max_payload) {
        throw ProtocolError("a frame of " + std::to_string(*length) + " bytes is too large");
    }
    return *length;
}

bool valid_id(const Json& id) {
    return id.is_number_integer() || id.is_number_unsigned() || id.is_string();
}

}  // namespace

// --- Errors --------------------------------------------------------------------------------------

Json RpcError::to_json() const {
    Json json = {{"code", code}, {"message", message}};
    if (!data.is_null()) {
        json["data"] = data;
    }
    return json;
}

RpcException::RpcException(RpcError error)
    : std::runtime_error(error.message), error_(std::move(error)) {}

// --- Frames --------------------------------------------------------------------------------------

std::string frame(std::string_view payload) {
    std::string text = "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    text += payload;
    return text;
}

std::vector<std::string> FrameReader::feed(std::string_view bytes) {
    buffer_ += bytes;
    std::vector<std::string> payloads;
    std::size_t start = 0;
    for (;;) {
        if (!length_) {
            const std::size_t end = buffer_.find("\r\n\r\n", start);
            if (end == std::string::npos) {
                if (buffer_.size() - start > max_header) {
                    throw ProtocolError("a frame header is too long or has no end");
                }
                break;
            }
            length_ = content_length(std::string_view(buffer_).substr(start, end - start));
            start = end + 4;
        }
        if (buffer_.size() - start < *length_) {
            break;
        }
        payloads.push_back(buffer_.substr(start, *length_));
        start += *length_;
        length_.reset();
    }
    buffer_.erase(0, start);
    return payloads;
}

// --- Messages ------------------------------------------------------------------------------------

Message Message::request(Json id, std::string method, Json params) {
    Message message;
    message.kind = Kind::request;
    message.id = std::move(id);
    message.method = std::move(method);
    message.params = std::move(params);
    return message;
}

Message Message::notification(std::string method, Json params) {
    Message message;
    message.kind = Kind::notification;
    message.method = std::move(method);
    message.params = std::move(params);
    return message;
}

Message Message::success(Json id, Json result) {
    Message message;
    message.kind = Kind::response;
    message.id = std::move(id);
    message.result = std::move(result);
    return message;
}

Message Message::failure(Json id, RpcError error) {
    Message message;
    message.kind = Kind::response;
    message.id = std::move(id);
    message.error = std::move(error);
    return message;
}

Message Message::parse(std::string_view payload) {
    Json json;
    try {
        json = Json::parse(payload);
    } catch (const Json::exception& e) {
        throw ProtocolError(std::string("not JSON: ") + e.what());
    }
    if (!json.is_object()) {
        throw ProtocolError("a message must be a JSON object");
    }
    if (json.value("jsonrpc", Json()) != "2.0") {
        throw ProtocolError("\"jsonrpc\" must be \"2.0\"");
    }
    Message message;
    const auto method = json.find("method");
    const auto id = json.find("id");
    if (method != json.end()) {
        if (!method->is_string()) {
            throw ProtocolError("\"method\" must be a string");
        }
        message.method = method->get<std::string>();
        if (const auto params = json.find("params"); params != json.end()) {
            if (!params->is_object() && !params->is_array()) {
                throw ProtocolError("\"params\" must be an object or an array");
            }
            message.params = *params;
        }
        if (id == json.end()) {
            message.kind = Kind::notification;
            return message;
        }
        if (!valid_id(*id)) {
            throw ProtocolError("\"id\" must be a number or a string");
        }
        message.kind = Kind::request;
        message.id = *id;
        return message;
    }
    if (id == json.end() || (!valid_id(*id) && !id->is_null())) {
        throw ProtocolError("a response needs an \"id\"");
    }
    message.kind = Kind::response;
    message.id = *id;
    const auto result = json.find("result");
    const auto error = json.find("error");
    if ((result == json.end()) == (error == json.end())) {
        throw ProtocolError("a response has either \"result\" or \"error\"");
    }
    if (result != json.end()) {
        message.result = *result;
        return message;
    }
    if (!error->is_object() || !error->contains("code") || !(*error)["code"].is_number_integer() ||
        !error->contains("message") || !(*error)["message"].is_string()) {
        throw ProtocolError("\"error\" needs an integer \"code\" and a string \"message\"");
    }
    RpcError parsed;
    parsed.code = (*error)["code"].get<int>();
    parsed.message = (*error)["message"].get<std::string>();
    parsed.data = error->value("data", Json());
    message.error = std::move(parsed);
    return message;
}

std::string Message::dump() const {
    Json json = {{"jsonrpc", "2.0"}};
    switch (kind) {
        case Kind::request:
            json["id"] = id;
            [[fallthrough]];
        case Kind::notification:
            json["method"] = method;
            if (!params.is_null()) {
                json["params"] = params;
            }
            break;
        case Kind::response:
            json["id"] = id;
            if (error) {
                json["error"] = error->to_json();
            } else {
                json["result"] = result;
            }
            break;
    }
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

const Json& Response::value() const {
    if (error) {
        throw RpcException(*error);
    }
    return result;
}

// --- Peer ----------------------------------------------------------------------------------------

Peer::Peer(Writer writer, Handler handler)
    : writer_(std::move(writer)), handler_(std::move(handler)) {}

std::int64_t Peer::request(std::string method, Json params, ResponseCallback on_response) {
    if (!on_response) {
        on_response = [](const Response&) {};
    }
    std::int64_t id = 0;
    std::optional<std::string> reason;
    {
        const std::lock_guard lock(mutex_);
        id = next_id_++;
        if (closed_) {
            reason = close_reason_;
        } else {
            waiting_.emplace(id, std::move(on_response));
        }
    }
    if (reason) {
        on_response(Response{nullptr, RpcError{rpc_error::connection_lost, *reason, nullptr}});
        return id;
    }
    send(Message::request(id, std::move(method), std::move(params)));
    return id;
}

void Peer::notify(std::string method, Json params) {
    send(Message::notification(std::move(method), std::move(params)));
}

void Peer::respond(const Json& id, Json result) {
    send(Message::success(id, std::move(result)));
}

void Peer::respond_error(const Json& id, RpcError error) {
    send(Message::failure(id, std::move(error)));
}

void Peer::send(const Message& message) {
    {
        const std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
    }
    // The writer is called outside the lock: it may block, and the reader must still take
    // responses meanwhile.
    writer_(frame(message.dump()));
}

bool Peer::feed(std::string_view bytes) {
    if (closed()) {
        return false;
    }
    std::vector<std::string> payloads;
    try {
        payloads = reader_.feed(bytes);
    } catch (const ProtocolError& e) {
        close(std::string("the module broke the protocol: ") + e.what());
        return false;
    }
    for (const std::string& payload : payloads) {
        Message message;
        try {
            message = Message::parse(payload);
        } catch (const ProtocolError& e) {
            // An id may still be readable: answer it, as JSON-RPC asks.
            Json id;
            try {
                const Json json = Json::parse(payload);
                if (json.is_object() && json.contains("method") && json.contains("id") &&
                    valid_id(json["id"])) {
                    id = json["id"];
                }
            } catch (...) {
                // Not JSON at all.
            }
            if (!id.is_null()) {
                respond_error(id, RpcError{rpc_error::invalid_request, e.what(), nullptr});
            }
            continue;
        }
        if (message.kind == Message::Kind::response) {
            ResponseCallback callback;
            {
                const std::lock_guard lock(mutex_);
                if (message.id.is_number_integer()) {
                    const auto it = waiting_.find(message.id.get<std::int64_t>());
                    if (it != waiting_.end()) {
                        callback = std::move(it->second);
                        waiting_.erase(it);
                    }
                }
            }
            if (callback) {
                callback(Response{std::move(message.result), std::move(message.error)});
            }
            continue;  // A response to nothing we asked is dropped.
        }
        handler_(std::move(message));
        if (closed()) {
            return false;
        }
    }
    return !closed();
}

void Peer::close(const std::string& reason) {
    std::map<std::int64_t, ResponseCallback> waiting;
    std::string message = reason.empty() ? "the connection is closed" : reason;
    {
        const std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        close_reason_ = message;
        waiting.swap(waiting_);
    }
    for (auto& [id, callback] : waiting) {
        callback(Response{nullptr, RpcError{rpc_error::connection_lost, message, nullptr}});
    }
}

bool Peer::closed() const {
    const std::lock_guard lock(mutex_);
    return closed_;
}

std::size_t Peer::waiting() const {
    const std::lock_guard lock(mutex_);
    return waiting_.size();
}

}  // namespace fem::ipc
