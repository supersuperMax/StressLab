// A process module for the extension host tests (tests/unit/core/extension/host_runtime_test.cpp):
// an executable that speaks the protocol of modules (docs/extensions/protocol.md) itself, the way
// a module written in another language would. Single-threaded: while a command runs it reads
// nothing, so a command that hangs also leaves pings unanswered.
//
// Commands: proc.echo returns its arguments; proc.mesh creates a mesh with arrays written inline
// (it declares no shared memory); proc.crash ends the process; proc.hang never returns;
// proc.stop_answering returns, then stops reading.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using nlohmann::json;

[[noreturn]] void sleep_forever() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}

/// The next message, or nothing at the end of the input.
std::optional<json> read_message() {
    std::string header;
    while (header.size() < 4 || header.compare(header.size() - 4, 4, "\r\n\r\n") != 0) {
        const int c = std::fgetc(stdin);
        if (c == EOF) {
            return std::nullopt;
        }
        header += static_cast<char>(c);
    }
    const std::string key = "Content-Length:";
    const std::size_t at = header.find(key);
    if (at == std::string::npos) {
        std::exit(2);
    }
    const std::size_t length = std::stoul(header.substr(at + key.size()));
    std::string payload(length, '\0');
    if (std::fread(payload.data(), 1, length, stdin) != length) {
        return std::nullopt;
    }
    return json::parse(payload);
}

void write_message(json message) {
    message["jsonrpc"] = "2.0";
    const std::string payload = message.dump();
    std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", payload.size());
    std::fwrite(payload.data(), 1, payload.size(), stdout);
    std::fflush(stdout);
}

void respond(const json& request, json result) {
    write_message({{"id", request["id"]}, {"result", std::move(result)}});
}

int next_id = 1000;

/// Sends a request to the application and returns its answer, answering pings meanwhile.
json request(const std::string& method, json params) {
    const int id = next_id++;
    write_message({{"id", id}, {"method", method}, {"params", std::move(params)}});
    for (;;) {
        std::optional<json> message = read_message();
        if (!message) {
            std::exit(0);
        }
        if (message->value("method", std::string()) == "$/ping") {
            respond(*message, json::object());
            continue;
        }
        if (!message->contains("method") && message->value("id", -1) == id) {
            return *message;
        }
    }
}

void execute(const json& message) {
    const json& params = message["params"];
    const std::string command = params["command"];
    if (command == "proc.echo") {
        respond(message, {{"value", params["args"]}});
    } else if (command == "proc.mesh") {
        json block = json::object();
        block["type"] = "line2";
        block["ids"] = json::array({1, 2});
        block["connectivity"] = json::array({0, 1, 1, 2});
        json nodes = json::object();
        nodes["ids"] = json::array({1, 2, 3});
        nodes["x"] = json::array({0.0, 1.0, 2.0});
        nodes["y"] = json::array({0.0, 0.0, 0.0});
        nodes["z"] = json::array({0.0, 0.0, 0.0});
        json data = json::object();
        data["kind"] = "mesh";
        data["nodes"] = nodes;
        data["blocks"] = json::array({block});
        const json answer = request("call/createObject", {{"call", params["call"]},
                                                          {"type", "mesh"},
                                                          {"owner", "fem.core"},
                                                          {"name", "inline"},
                                                          {"data", data}});
        if (answer.contains("error")) {
            write_message({{"id", message["id"]}, {"error", answer["error"]}});
            return;
        }
        respond(message, {{"value", answer["result"]["id"]}});
    } else if (command == "proc.crash") {
        std::fputs("process module: crashing\n", stderr);
        std::exit(9);
    } else if (command == "proc.hang") {
        sleep_forever();
    } else if (command == "proc.stop_answering") {
        respond(message, {{"value", true}});
        sleep_forever();
    } else {
        write_message(
            {{"id", message["id"]}, {"error", {{"code", -32601}, {"message", "no such command"}}}});
    }
}

}  // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (;;) {
        std::optional<json> message = read_message();
        if (!message) {
            return 0;
        }
        const std::string method = message->value("method", std::string());
        if (method == "initialize") {
            json commands = json::array(
                {"proc.echo", "proc.mesh", "proc.crash", "proc.hang", "proc.stop_answering"});
            request("contributions/register", {{"commands", commands}});
            respond(*message, {{"capabilities", {{"sharedMemory", false}}}});
        } else if (method == "$/ping") {
            respond(*message, json::object());
        } else if (method == "command/execute") {
            execute(*message);
        } else if (method == "shutdown") {
            respond(*message, nullptr);
        } else if (method == "exit") {
            return 0;
        } else if (message->contains("id") && message->contains("method")) {
            write_message({{"id", (*message)["id"]},
                           {"error", {{"code", -32601}, {"message", "unknown method"}}}});
        }
    }
}
