#include "fem/core/context/context_keys.h"
#include "fem/core/context/error.h"
#include "fem/core/context/when.h"
#include "fem/core/signal.h"

#include "context_test_support.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::Subscription;
using fem::context::ContextErrc;
using fem::context::ContextKeys;
using fem::context::ContextValue;
using fem::context::WhenExpression;
using Kind = ContextValue::Kind;

constexpr const char* owner = "test.module";

WhenExpression when(std::string_view text) {
    return WhenExpression::parse(text);
}

/// Values a listener received.
struct Calls {
    std::vector<bool> values;

    [[nodiscard]] std::function<void(bool)> listener() {
        return [this](bool value) { values.push_back(value); };
    }
};

class WatchTest : public ::testing::Test {
protected:
    void declare(const std::string& name, ContextValue initial = false) {
        keys.declare({.name = name, .owner = owner, .type = Kind::boolean, .initial = initial});
    }
    /// Evaluations caused by `change`.
    template <class Change>
    std::uint64_t evaluations(Change&& change) {
        const std::uint64_t before = keys.watch_evaluations();
        change();
        return keys.watch_evaluations() - before;
    }

    ContextKeys keys;
};

TEST_F(WatchTest, ListenerIsCalledWhenTheValueChanges) {
    declare("a");
    Calls calls;
    const Subscription watch = keys.watch(when("a"), calls.listener());
    EXPECT_TRUE(calls.values.empty());

    keys.set("a", true);
    keys.set("a", true);
    keys.set("a", false);
    keys.set("a", {});  // No value is still false.
    EXPECT_EQ(calls.values, (std::vector<bool>{true, false}));
}

TEST_F(WatchTest, ChangeOfAKeyThatKeepsTheValueIsNotReported) {
    declare("a", true);
    declare("b");
    Calls calls;
    const Subscription watch = keys.watch(when("a || b"), calls.listener());

    EXPECT_EQ(evaluations([&] { keys.set("b", true); }), 1U);
    EXPECT_TRUE(calls.values.empty());
}

TEST_F(WatchTest, OnlyExpressionsThatReadTheKeyAreEvaluated) {
    constexpr int count = 1000;
    std::vector<Calls> calls(count);
    std::vector<Subscription> watches;
    for (int i = 0; i < count; ++i) {
        declare("k" + std::to_string(i));
        watches.push_back(keys.watch(when("k" + std::to_string(i)), calls[i].listener()));
    }
    // Ten more expressions read k0.
    Calls shared;
    for (int i = 1; i <= 10; ++i) {
        watches.push_back(keys.watch(when("k0 && !k" + std::to_string(i)), shared.listener()));
    }

    EXPECT_EQ(evaluations([&] { keys.set("k500", true); }), 1U);
    EXPECT_EQ(calls[500].values, std::vector<bool>{true});
    EXPECT_TRUE(calls[499].values.empty());
    EXPECT_TRUE(calls[501].values.empty());

    EXPECT_EQ(evaluations([&] { keys.set("k0", true); }), 11U);
    EXPECT_EQ(calls[0].values, std::vector<bool>{true});
    EXPECT_EQ(shared.values, std::vector<bool>(10, true));

    // k3 is read by its own expression and by one of the ten.
    EXPECT_EQ(evaluations([&] { keys.set("k3", true); }), 2U);
    EXPECT_EQ(shared.values.size(), 11U);
}

TEST_F(WatchTest, NothingIsEvaluatedWhenNoValueChanges) {
    declare("a");
    declare("b");
    Calls calls;
    const Subscription watch = keys.watch(when("a"), calls.listener());

    EXPECT_EQ(evaluations([&] { keys.set("a", false); }), 0U);
    EXPECT_EQ(evaluations([&] { keys.set("b", true); }), 0U);
    EXPECT_EQ(evaluations([&] { keys.set({{"a", false}, {"b", false}}); }), 0U);
}

TEST_F(WatchTest, StoppedWatchIsNotEvaluated) {
    declare("a");
    Calls calls;
    Subscription watch = keys.watch(when("a"), calls.listener());
    watch.reset();

    EXPECT_EQ(evaluations([&] { keys.set("a", true); }), 0U);
    EXPECT_TRUE(calls.values.empty());
}

TEST_F(WatchTest, SeveralKeysAtOnceEvaluateEachExpressionOnce) {
    declare("a");
    declare("b");
    Calls calls;
    const Subscription watch = keys.watch(when("a && b"), calls.listener());

    EXPECT_EQ(evaluations([&] { keys.set({{"a", true}, {"b", true}}); }), 1U);
    EXPECT_EQ(calls.values, std::vector<bool>{true});
}

TEST_F(WatchTest, SeveralKeysAtOnceShowNoIntermediateState) {
    keys.declare(
        {.name = "selection.type", .owner = owner, .type = Kind::string, .initial = "edge"});
    keys.declare({.name = "selection.count", .owner = owner, .type = Kind::number, .initial = 1});
    const WhenExpression single_face = when("selection.type == 'face' && selection.count == 1");

    Calls one_by_one;
    {
        const Subscription watch = keys.watch(single_face, one_by_one.listener());
        keys.set("selection.type", "face");
        keys.set("selection.count", 2);
    }
    EXPECT_EQ(one_by_one.values, (std::vector<bool>{true, false}));

    keys.set({{"selection.type", "edge"}, {"selection.count", 1}});
    Calls at_once;
    const Subscription watch = keys.watch(single_face, at_once.listener());
    keys.set({{"selection.type", "face"}, {"selection.count", 2}});
    EXPECT_TRUE(at_once.values.empty());
}

TEST_F(WatchTest, KeysOfAModuleAppearAndDisappearWithIt) {
    Calls ready;
    Calls not_ready;
    const Subscription watch_ready = keys.watch(when("ext.ready"), ready.listener());
    const Subscription watch_not_ready = keys.watch(when("!ext.ready"), not_ready.listener());
    EXPECT_FALSE(keys.evaluate(when("ext.ready")));

    keys.declare({.name = "ext.ready", .owner = "test.ext", .initial = true});
    EXPECT_EQ(ready.values, std::vector<bool>{true});
    EXPECT_EQ(not_ready.values, std::vector<bool>{false});

    EXPECT_EQ(evaluations([&] { keys.remove_owner("test.ext"); }), 2U);
    EXPECT_EQ(ready.values, (std::vector<bool>{true, false}));
    EXPECT_EQ(not_ready.values, (std::vector<bool>{false, true}));

    // A key declared without a value changes nothing.
    EXPECT_EQ(evaluations([&] { keys.declare({.name = "ext.ready", .owner = "test.ext"}); }), 0U);
}

TEST_F(WatchTest, ListenerMaySetKeys) {
    declare("a");
    declare("b");
    Calls calls;
    const Subscription mirror = keys.watch(when("a"), [&](bool value) { keys.set("b", value); });
    const Subscription watch = keys.watch(when("b"), calls.listener());

    keys.set("a", true);
    EXPECT_EQ(calls.values, std::vector<bool>{true});
    EXPECT_EQ(keys.value("b"), ContextValue(true));
}

TEST_F(WatchTest, ChangeMadeByAListenerIsNotReportedOutOfDate) {
    declare("a");
    declare("c");
    // The first listener sets c, which makes the second expression false again before its
    // listener is called for the change of a.
    const Subscription first = keys.watch(when("a"), [&](bool value) {
        if (value) {
            keys.set("c", true);
        }
    });
    Calls calls;
    const WhenExpression second = when("a && !c");
    const Subscription watch = keys.watch(second, calls.listener());

    keys.set("a", true);
    EXPECT_TRUE(calls.values.empty());
    EXPECT_FALSE(keys.evaluate(second));
}

TEST_F(WatchTest, ListenerMayStopAnotherWatch) {
    declare("a");
    Calls calls;
    Subscription second;
    const Subscription first = keys.watch(when("a"), [&](bool) { second.reset(); });
    second = keys.watch(when("a"), calls.listener());

    keys.set("a", true);
    EXPECT_TRUE(calls.values.empty());
}

TEST_F(WatchTest, WatchStartedByAListenerReportsLaterChanges) {
    declare("a");
    declare("b");
    Calls calls;
    Subscription late;
    bool started = false;
    const Subscription first = keys.watch(when("a"), [&](bool) {
        if (!started) {
            started = true;
            late = keys.watch(when("a || b"), calls.listener());
        }
    });

    keys.set("a", true);
    EXPECT_TRUE(calls.values.empty());
    keys.set("a", false);
    EXPECT_EQ(calls.values, std::vector<bool>{false});
}

TEST_F(WatchTest, ListenerAfterOneThatThrowsIsCalledOnALaterChange) {
    declare("a");
    declare("c");
    const Subscription first = keys.watch(when("a"), [](bool value) {
        if (value) {
            throw std::runtime_error("listener failed");
        }
    });
    Calls calls;
    const Subscription second = keys.watch(when("a || c"), calls.listener());

    EXPECT_THROW(keys.set("a", true), std::runtime_error);
    EXPECT_EQ(keys.value("a"), ContextValue(true));
    EXPECT_TRUE(calls.values.empty());

    keys.set("c", true);
    EXPECT_EQ(calls.values, std::vector<bool>{true});
}

TEST_F(WatchTest, SubscriptionMayOutliveTheKeys) {
    auto owned = std::make_unique<ContextKeys>();
    Subscription watch = owned->watch(when("a"), [](bool) {});
    owned.reset();
    watch.reset();
}

TEST_F(WatchTest, EmptyListenerIsRefused) {
    EXPECT_CONTEXT_ERROR((void)keys.watch(when("a"), nullptr), ContextErrc::invalid_spec);
}

}  // namespace
