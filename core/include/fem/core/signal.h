#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace fem {

/// Keeps a listener connected to a `Signal` and disconnects it when destroyed or reset.
/// It may outlive the signal; then destroying it does nothing.
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> disconnect) : disconnect_(std::move(disconnect)) {}

    Subscription(Subscription&& other) noexcept : disconnect_(std::move(other.disconnect_)) {
        other.disconnect_ = nullptr;
    }
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            disconnect_ = std::move(other.disconnect_);
            other.disconnect_ = nullptr;
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }

    /// Disconnects the listener now; does nothing if it is already disconnected.
    void reset() noexcept {
        if (disconnect_) {
            const std::function<void()> disconnect = std::move(disconnect_);
            disconnect_ = nullptr;
            disconnect();
        }
    }

private:
    std::function<void()> disconnect_;
};

/// Listeners called in the order they subscribed. Single-threaded, like the model.
///
/// Listeners may subscribe and unsubscribe while the signal is emitted: a new listener is first
/// called by the next emission, a removed one is not called any more. An exception thrown by a
/// listener propagates to the code that emitted the signal, and later listeners are not called.
template <class... Args>
class Signal {
public:
    using Listener = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(Signal&&) = delete;
    ~Signal() = default;

    [[nodiscard]] Subscription subscribe(Listener listener) {
        auto slot = std::make_shared<Slot>(Slot{std::move(listener), true});
        slots_->push_back(slot);
        return Subscription(
            [weak_slots = std::weak_ptr<Slots>(slots_), weak_slot = std::weak_ptr<Slot>(slot)] {
                const std::shared_ptr<Slot> slot = weak_slot.lock();
                if (!slot) {
                    return;
                }
                slot->active = false;
                if (const std::shared_ptr<Slots> slots = weak_slots.lock()) {
                    std::erase(*slots, slot);
                }
            });
    }

    void emit(Args... args) const {
        const Slots snapshot = *slots_;
        for (const std::shared_ptr<Slot>& slot : snapshot) {
            if (slot->active) {
                slot->listener(args...);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return slots_->size(); }

private:
    struct Slot {
        Listener listener;
        bool active;
    };
    using Slots = std::vector<std::shared_ptr<Slot>>;

    std::shared_ptr<Slots> slots_ = std::make_shared<Slots>();
};

}  // namespace fem
