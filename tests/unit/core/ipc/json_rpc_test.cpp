// JSON-RPC 2.0 in LSP frames (task 1.9): cutting the stream, messages, matching responses.

#include "fem/core/ipc/json_rpc.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

using fem::ipc::frame;
using fem::ipc::FrameReader;
using fem::ipc::Json;
using fem::ipc::Message;
using fem::ipc::Peer;
using fem::ipc::ProtocolError;
using fem::ipc::Response;
using fem::ipc::RpcError;
using fem::ipc::RpcException;
namespace rpc_error = fem::ipc::rpc_error;

// --- Frames --------------------------------------------------------------------------------------

TEST(FrameReader, ReadsFramesSplitAnywhere) {
    const std::string stream = frame(R"({"a":1})") + frame("{\"текст\":\"да\"}") + frame("{}");
    for (std::size_t piece = 1; piece <= stream.size(); ++piece) {
        FrameReader reader;
        std::vector<std::string> payloads;
        for (std::size_t at = 0; at < stream.size(); at += piece) {
            for (std::string& payload : reader.feed(std::string_view(stream).substr(at, piece))) {
                payloads.push_back(std::move(payload));
            }
        }
        ASSERT_EQ(payloads, (std::vector<std::string>{R"({"a":1})", "{\"текст\":\"да\"}", "{}"}))
            << "pieces of " << piece;
        EXPECT_EQ(reader.buffered(), 0U);
    }
}

TEST(FrameReader, CountsBytesNotCharacters) {
    EXPECT_EQ(frame("ё"), "Content-Length: 2\r\n\r\nё");
}

TEST(FrameReader, IgnoresOtherHeadersAndCaseOfNames) {
    FrameReader reader;
    const auto payloads = reader.feed(
        "content-type: application/vscode-jsonrpc; charset=utf-8\r\nCONTENT-LENGTH:  2 \r\n\r\n{}");
    EXPECT_EQ(payloads, std::vector<std::string>{"{}"});
}

TEST(FrameReader, KeepsIncompleteFrame) {
    FrameReader reader;
    EXPECT_TRUE(reader.feed("Content-Length: 10\r\n\r\n{\"a\":").empty());
    EXPECT_EQ(reader.feed("12}  "), std::vector<std::string>{"{\"a\":12}  "});
}

TEST(FrameReader, RefusesMalformedHeaders) {
    for (const std::string bad :
         {"Content-Length: x\r\n\r\n", "Content-Length: -1\r\n\r\n", "no colon\r\n\r\n",
          "Content-Type: a\r\n\r\n", "Content-Length: 999999999999\r\n\r\n"}) {
        FrameReader reader;
        EXPECT_THROW((void)reader.feed(bad), ProtocolError) << bad;
    }
    FrameReader endless;
    EXPECT_THROW((void)endless.feed(std::string(5000, 'a')), ProtocolError);
}

// --- Messages ------------------------------------------------------------------------------------

TEST(Message, ParsesTheFourKinds) {
    const Message request =
        Message::parse(R"({"jsonrpc":"2.0","id":7,"method":"m","params":{"a":1}})");
    EXPECT_EQ(request.kind, Message::Kind::request);
    EXPECT_EQ(request.id, 7);
    EXPECT_EQ(request.method, "m");
    EXPECT_EQ(request.params["a"], 1);

    const Message notification = Message::parse(R"({"jsonrpc":"2.0","method":"n"})");
    EXPECT_EQ(notification.kind, Message::Kind::notification);
    EXPECT_TRUE(notification.params.is_null());

    const Message success = Message::parse(R"({"jsonrpc":"2.0","id":"s","result":null})");
    EXPECT_EQ(success.kind, Message::Kind::response);
    EXPECT_FALSE(success.error);

    const Message failure = Message::parse(
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"no","data":[1]}})");
    ASSERT_TRUE(failure.error);
    EXPECT_EQ(failure.error->code, rpc_error::method_not_found);
    EXPECT_EQ(failure.error->message, "no");
    EXPECT_EQ(failure.error->data, Json::array({1}));
}

TEST(Message, DumpsWhatParseReads) {
    for (const Message& message :
         {Message::request(1, "a/b", {{"x", "й"}}), Message::notification("c"),
          Message::success("id", Json::array({1, 2})),
          Message::failure(5, RpcError{rpc_error::command_failed, "failed", nullptr})}) {
        const Message back = Message::parse(message.dump());
        EXPECT_EQ(back.kind, message.kind);
        EXPECT_EQ(back.id, message.id);
        EXPECT_EQ(back.method, message.method);
        EXPECT_EQ(back.params, message.params);
        EXPECT_EQ(back.result, message.result);
        EXPECT_EQ(back.error.has_value(), message.error.has_value());
        EXPECT_EQ(back.dump(), message.dump());
    }
    EXPECT_EQ(Message::notification("n").dump(), R"({"jsonrpc":"2.0","method":"n"})");
}

TEST(Message, RefusesWhatIsNotJsonRpc) {
    for (const std::string bad :
         {"not json", "[1]", R"({"id":1,"method":"m"})", R"({"jsonrpc":"1.0","method":"m"})",
          R"({"jsonrpc":"2.0","method":5})", R"({"jsonrpc":"2.0","method":"m","params":3})",
          R"({"jsonrpc":"2.0","id":{},"method":"m"})", R"({"jsonrpc":"2.0","id":1})",
          R"({"jsonrpc":"2.0","id":1,"result":1,"error":{"code":1,"message":""}})",
          R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":""}})"}) {
        EXPECT_THROW((void)Message::parse(bad), ProtocolError) << bad;
    }
}

TEST(Response, ValueThrowsTheError) {
    const Response failed{nullptr, RpcError{rpc_error::not_found, "gone", nullptr}};
    try {
        (void)failed.value();
        FAIL();
    } catch (const RpcException& e) {
        EXPECT_EQ(e.error().code, rpc_error::not_found);
        EXPECT_STREQ(e.what(), "gone");
    }
    EXPECT_EQ((Response{42, std::nullopt}.value()), 42);
}

// --- Peer ----------------------------------------------------------------------------------------

/// Two peers wired to each other in memory, on the calling thread.
struct Pair {
    std::vector<Message> received_by_a;
    std::vector<Message> received_by_b;
    Peer* a_ptr = nullptr;
    Peer* b_ptr = nullptr;
    Peer a{[this](std::string bytes) { return b_ptr->feed(bytes); },
           [this](Message m) { received_by_a.push_back(std::move(m)); }};
    Peer b{[this](std::string bytes) { return a_ptr->feed(bytes); },
           [this](Message m) {
               if (m.method == "add" && m.kind == Message::Kind::request) {
                   b_ptr->respond(m.id, m.params[0].get<int>() + m.params[1].get<int>());
               } else if (m.method == "fail" && m.kind == Message::Kind::request) {
                   b_ptr->respond_error(m.id, RpcError{rpc_error::command_failed, "no", nullptr});
               } else {
                   received_by_b.push_back(std::move(m));
               }
           }};

    Pair() {
        a_ptr = &a;
        b_ptr = &b;
    }
};

TEST(Peer, MatchesResponsesToRequests) {
    Pair pair;
    std::vector<int> sums;
    pair.a.request("add", Json::array({2, 3}), [&](Response r) { sums.push_back(r.value()); });
    pair.a.request("add", Json::array({10, 20}), [&](Response r) { sums.push_back(r.value()); });
    std::optional<RpcError> error;
    pair.a.request("fail", nullptr, [&](Response r) { error = r.error; });
    EXPECT_EQ(sums, (std::vector<int>{5, 30}));
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, rpc_error::command_failed);
    EXPECT_EQ(pair.a.waiting(), 0U);
}

TEST(Peer, HandsNotificationsAndRequestsToTheHandler) {
    Pair pair;
    pair.a.notify("hello", {{"to", "b"}});
    pair.a.request("other", nullptr, nullptr);
    ASSERT_EQ(pair.received_by_b.size(), 2U);
    EXPECT_EQ(pair.received_by_b[0].kind, Message::Kind::notification);
    EXPECT_EQ(pair.received_by_b[0].params["to"], "b");
    EXPECT_EQ(pair.received_by_b[1].kind, Message::Kind::request);
    EXPECT_EQ(pair.a.waiting(), 1U);  // Nobody answered it.
}

TEST(Peer, CloseAnswersWaitingRequests) {
    std::vector<std::string> sent;
    Peer peer(
        [&](std::string bytes) {
            sent.push_back(std::move(bytes));
            return true;
        },
        [](Message) {});
    std::optional<RpcError> first;
    peer.request("m", nullptr, [&](Response r) { first = r.error; });
    peer.close("the module crashed");
    peer.close("again");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->code, rpc_error::connection_lost);
    EXPECT_EQ(first->message, "the module crashed");
    std::optional<RpcError> late;
    peer.request("m", nullptr, [&](Response r) { late = r.error; });
    ASSERT_TRUE(late);
    EXPECT_EQ(late->message, "the module crashed");
    peer.notify("n");
    EXPECT_EQ(sent.size(), 1U);  // Nothing is sent after the close.
    EXPECT_TRUE(peer.closed());
    EXPECT_FALSE(peer.feed(frame(R"({"jsonrpc":"2.0","method":"n"})")));
}

TEST(Peer, AnswersBadRequestsWithAnError) {
    std::vector<Message> sent;
    std::vector<Message> handled;
    Peer peer(
        [&](std::string bytes) {
            FrameReader reader;
            for (const std::string& payload : reader.feed(bytes)) {
                sent.push_back(Message::parse(payload));
            }
            return true;
        },
        [&](Message m) { handled.push_back(std::move(m)); });
    EXPECT_TRUE(peer.feed(frame(R"({"jsonrpc":"1.0","id":4,"method":"m"})")));
    EXPECT_TRUE(peer.feed(frame("garbage")));
    EXPECT_TRUE(peer.feed(frame(R"({"jsonrpc":"2.0","id":99,"result":1})")));  // Not asked.
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].id, 4);
    EXPECT_EQ(sent[0].error->code, rpc_error::invalid_request);
    EXPECT_TRUE(handled.empty());
}

TEST(Peer, MalformedFrameClosesConnection) {
    Peer peer([](std::string) { return true; }, [](Message) {});
    std::optional<RpcError> error;
    peer.request("m", nullptr, [&](Response r) { error = r.error; });
    EXPECT_FALSE(peer.feed("Content-Length: nope\r\n\r\n"));
    EXPECT_TRUE(peer.closed());
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code, rpc_error::connection_lost);
}

TEST(Peer, SendsFromManyThreads) {
    std::mutex mutex;
    FrameReader reader;
    int messages = 0;
    Peer peer(
        [&](std::string bytes) {
            const std::lock_guard lock(mutex);
            messages += static_cast<int>(reader.feed(bytes).size());
            return true;
        },
        [](Message) {});
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                peer.notify("n", {{"i", i}});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(messages, 1600);
}

}  // namespace
