#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// JSON-RPC 2.0 (https://www.jsonrpc.org/specification) in frames of the Language Server Protocol:
// a header "Content-Length: <bytes>\r\n", an empty line, then the message as UTF-8 JSON. The
// application talks this way to out-of-process modules (docs/extensions/protocol.md).

namespace fem::ipc {

using Json = nlohmann::json;

/// Error codes of JSON-RPC 2.0 and of the protocol of modules (the range -32000..-32099 is for
/// the server).
namespace rpc_error {
inline constexpr int parse_error = -32700;
inline constexpr int invalid_request = -32600;
inline constexpr int method_not_found = -32601;
inline constexpr int invalid_params = -32602;
inline constexpr int internal_error = -32603;
/// The code of the module, or a command it ran, reported an error.
inline constexpr int command_failed = -32001;
/// The work was cancelled.
inline constexpr int cancelled = -32002;
/// The other side is gone: never sent, given to requests that will not get an answer.
inline constexpr int connection_lost = -32003;
/// An object, call or task named in the request does not exist (any more).
inline constexpr int not_found = -32004;
}  // namespace rpc_error

/// The error member of a response.
struct RpcError {
    int code = rpc_error::internal_error;
    std::string message;
    Json data;  ///< Null if absent.

    [[nodiscard]] Json to_json() const;
};

/// A request answered with an error, as an exception for code that waits for the answer.
class RpcException : public std::runtime_error {
public:
    explicit RpcException(RpcError error);
    [[nodiscard]] const RpcError& error() const noexcept { return error_; }

private:
    RpcError error_;
};

/// The byte stream is not JSON-RPC in LSP frames; the connection cannot go on.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// "Content-Length: <size>\r\n\r\n<payload>".
[[nodiscard]] std::string frame(std::string_view payload);

/// Cuts a byte stream into the payloads of frames. Headers other than Content-Length (e.g.
/// Content-Type) are ignored; header names are compared ignoring ASCII case.
class FrameReader {
public:
    /// Largest payload accepted; arrays go through shared memory, not messages.
    static constexpr std::size_t max_payload = std::size_t{256} << 20U;

    /// Takes the next bytes of the stream and returns the payloads now complete, in order.
    /// Throws ProtocolError on a malformed header; the reader is unusable afterwards.
    std::vector<std::string> feed(std::string_view bytes);
    /// Bytes received but not yet a whole frame.
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }

private:
    std::string buffer_;
    std::optional<std::size_t> length_;  // Of the payload whose header has been read.
};

/// A JSON-RPC 2.0 message.
struct Message {
    enum class Kind : std::uint8_t { request, notification, response };

    Kind kind = Kind::notification;
    /// Of a request or a response: a number or a string.
    Json id;
    /// Of a request or a notification.
    std::string method;
    /// Of a request or a notification: an object, an array or null when absent.
    Json params;
    /// Of a successful response.
    Json result;
    /// Of a failed response.
    std::optional<RpcError> error;

    [[nodiscard]] static Message request(Json id, std::string method, Json params = nullptr);
    [[nodiscard]] static Message notification(std::string method, Json params = nullptr);
    [[nodiscard]] static Message success(Json id, Json result);
    [[nodiscard]] static Message failure(Json id, RpcError error);

    /// Throws ProtocolError(…) with the reason if the payload is not a JSON-RPC 2.0 message.
    /// Batches are not supported.
    [[nodiscard]] static Message parse(std::string_view payload);
    /// Compact JSON, UTF-8 as is.
    [[nodiscard]] std::string dump() const;
};

/// The answer to a request: its result, or its error.
struct Response {
    Json result;
    std::optional<RpcError> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
    /// The result; throws RpcException with the error.
    const Json& value() const;
};

/// One side of a JSON-RPC connection: sends messages through a writer, takes the bytes the other
/// side sends, matches responses to requests and hands everything else to a handler.
///
/// Thread-safe: requests may be sent on any thread; `feed` is called on one thread (the reader)
/// at a time, and the handler and the response callbacks run on it.
class Peer {
public:
    /// Writes one frame; true if it was written (or queued). Called on the sending thread.
    using Writer = std::function<bool(std::string frame)>;
    /// Gets requests and notifications of the other side.
    using Handler = std::function<void(Message message)>;
    using ResponseCallback = std::function<void(Response response)>;

    Peer(Writer writer, Handler handler);
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&) = delete;
    Peer& operator=(Peer&&) = delete;
    ~Peer() = default;

    /// Sends a request; `on_response` is called once, on the reader thread, with the answer, or
    /// with a `connection_lost` error when the connection closes first (on the closing thread;
    /// at once if it is closed already). Returns the id of the request.
    std::int64_t request(std::string method, Json params, ResponseCallback on_response);
    void notify(std::string method, Json params = nullptr);
    void respond(const Json& id, Json result);
    void respond_error(const Json& id, RpcError error);

    /// Takes bytes from the other side. A message that is not JSON-RPC is answered with an error
    /// if it has an id and is otherwise dropped; a malformed frame closes the connection. Returns
    /// false when the connection is closed.
    bool feed(std::string_view bytes);
    /// Answers every waiting request with a `connection_lost` error carrying `reason`; no more
    /// messages are sent or taken. Idempotent.
    void close(const std::string& reason);
    [[nodiscard]] bool closed() const;
    /// Requests sent and not answered yet.
    [[nodiscard]] std::size_t waiting() const;

private:
    void send(const Message& message);

    Writer writer_;
    Handler handler_;
    mutable std::mutex mutex_;
    std::int64_t next_id_ = 1;
    std::map<std::int64_t, ResponseCallback> waiting_;
    bool closed_ = false;
    std::string close_reason_;
    FrameReader reader_;  // Used by the reader thread only.
};

}  // namespace fem::ipc
