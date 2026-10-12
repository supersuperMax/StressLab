// ConcurrentSignal: a signal emitted on worker threads (task 1.6).

#include "fem/core/concurrent_signal.h"

#include "fem/core/signal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

TEST(ConcurrentSignal, CallsListenersInOrder) {
    fem::ConcurrentSignal<int> signal;
    std::vector<int> seen;
    const fem::Subscription a = signal.subscribe([&](int value) { seen.push_back(value); });
    const fem::Subscription b = signal.subscribe([&](int value) { seen.push_back(value * 10); });
    signal.emit(3);
    EXPECT_EQ(seen, (std::vector<int>{3, 30}));
    EXPECT_EQ(signal.size(), 2U);
}

TEST(ConcurrentSignal, ResetStopsCalls) {
    fem::ConcurrentSignal<> signal;
    int calls = 0;
    fem::Subscription subscription = signal.subscribe([&] { ++calls; });
    signal.emit();
    subscription.reset();
    signal.emit();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(signal.size(), 0U);
}

TEST(ConcurrentSignal, ListenerMayUnsubscribeItself) {
    fem::ConcurrentSignal<> signal;
    int calls = 0;
    std::optional<fem::Subscription> subscription;
    subscription = signal.subscribe([&] {
        ++calls;
        subscription->reset();
    });
    signal.emit();
    signal.emit();
    EXPECT_EQ(calls, 1);
}

TEST(ConcurrentSignal, SubscriptionMayOutliveSignal) {
    fem::Subscription subscription;
    {
        fem::ConcurrentSignal<> signal;
        subscription = signal.subscribe([] {});
    }
    subscription.reset();
}

TEST(ConcurrentSignal, ResetWaitsForCallOnAnotherThread) {
    fem::ConcurrentSignal<> signal;
    std::atomic<bool> inside = false;
    std::atomic<bool> finished = false;
    fem::Subscription subscription = signal.subscribe([&] {
        inside = true;
        std::this_thread::sleep_for(50ms);
        finished = true;
    });
    std::thread emitter([&] { signal.emit(); });
    while (!inside.load()) {
        std::this_thread::sleep_for(1ms);
    }
    subscription.reset();
    EXPECT_TRUE(finished.load()) << "reset returned while the listener was running";
    emitter.join();
}

TEST(ConcurrentSignal, EmitsFromManyThreads) {
    fem::ConcurrentSignal<int> signal;
    std::atomic<int> sum = 0;
    const fem::Subscription subscription = signal.subscribe([&](int value) { sum += value; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                signal.emit(1);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sum.load(), 4000);
}

}  // namespace
