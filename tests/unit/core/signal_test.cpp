#include "fem/core/signal.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using fem::Signal;
using fem::Subscription;

TEST(Signal, ListenersAreCalledInSubscriptionOrder) {
    Signal<int> signal;
    std::vector<std::string> calls;
    const Subscription first =
        signal.subscribe([&](int x) { calls.push_back("a" + std::to_string(x)); });
    const Subscription second =
        signal.subscribe([&](int x) { calls.push_back("b" + std::to_string(x)); });

    signal.emit(1);
    signal.emit(2);
    EXPECT_EQ(calls, (std::vector<std::string>{"a1", "b1", "a2", "b2"}));
    EXPECT_EQ(signal.size(), 2U);
}

TEST(Signal, DestroyingOrResettingTheSubscriptionDisconnects) {
    Signal<> signal;
    int calls = 0;
    {
        const Subscription scoped = signal.subscribe([&] { ++calls; });
        signal.emit();
    }
    signal.emit();
    EXPECT_EQ(calls, 1);

    Subscription manual = signal.subscribe([&] { ++calls; });
    manual.reset();
    manual.reset();
    signal.emit();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(signal.size(), 0U);
}

TEST(Signal, MovedSubscriptionKeepsTheListener) {
    Signal<> signal;
    int calls = 0;
    Subscription target;
    {
        Subscription source = signal.subscribe([&] { ++calls; });
        target = std::move(source);
    }
    signal.emit();
    EXPECT_EQ(calls, 1);
}

TEST(Signal, ListenersMayUnsubscribeDuringEmission) {
    Signal<> signal;
    std::vector<std::string> calls;
    Subscription second;
    const Subscription first = signal.subscribe([&] {
        calls.emplace_back("first");
        second.reset();  // Not called in this emission any more.
    });
    second = signal.subscribe([&] { calls.emplace_back("second"); });
    Subscription late;
    bool late_added = false;
    const Subscription third = signal.subscribe([&] {
        calls.emplace_back("third");
        if (!late_added) {
            late_added = true;
            late = signal.subscribe([&] { calls.emplace_back("late"); });
        }
    });

    signal.emit();
    EXPECT_EQ(calls, (std::vector<std::string>{"first", "third"}));
    signal.emit();
    EXPECT_EQ(calls, (std::vector<std::string>{"first", "third", "first", "third", "late"}));
}

TEST(Signal, SubscriptionMayOutliveTheSignal) {
    Subscription subscription;
    {
        Signal<> signal;
        subscription = signal.subscribe([] {});
    }
    subscription.reset();  // Must not touch the destroyed signal.
    SUCCEED();
}
