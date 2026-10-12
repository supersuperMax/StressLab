#pragma once

#include "fem/core/signal.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace fem {

/// Like `Signal`, but may be emitted, subscribed to and unsubscribed from on any thread. Used by
/// tasks and jobs, which change on worker threads.
///
/// Listeners are called on the thread that emits, outside of any lock of the signal, in the order
/// they subscribed. Once `Subscription::reset` returns, the listener is not running on another
/// thread and is not called again, so an object may unsubscribe in its destructor. A listener may
/// unsubscribe itself; it must not wait for a thread that is unsubscribing it at that moment.
template <class... Args>
class ConcurrentSignal {
public:
    using Listener = std::function<void(Args...)>;

    ConcurrentSignal() = default;
    ConcurrentSignal(const ConcurrentSignal&) = delete;
    ConcurrentSignal& operator=(const ConcurrentSignal&) = delete;
    ConcurrentSignal(ConcurrentSignal&&) = delete;
    ConcurrentSignal& operator=(ConcurrentSignal&&) = delete;
    ~ConcurrentSignal() = default;

    [[nodiscard]] Subscription subscribe(Listener listener) {
        auto slot = std::make_shared<Slot>();
        slot->listener = std::move(listener);
        {
            const std::lock_guard lock(state_->mutex);
            state_->slots.push_back(slot);
        }
        return Subscription(
            [weak_state = std::weak_ptr<State>(state_), weak_slot = std::weak_ptr<Slot>(slot)] {
                const std::shared_ptr<Slot> slot = weak_slot.lock();
                if (!slot) {
                    return;
                }
                {
                    // Waits for a call in progress on another thread.
                    const std::lock_guard call(slot->mutex);
                    slot->active = false;
                }
                if (const std::shared_ptr<State> state = weak_state.lock()) {
                    const std::lock_guard lock(state->mutex);
                    std::erase(state->slots, slot);
                }
            });
    }

    void emit(Args... args) const {
        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            const std::lock_guard lock(state_->mutex);
            snapshot = state_->slots;
        }
        for (const std::shared_ptr<Slot>& slot : snapshot) {
            const std::lock_guard call(slot->mutex);
            if (slot->active) {
                slot->listener(args...);
            }
        }
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(state_->mutex);
        return state_->slots.size();
    }

private:
    struct Slot {
        // Recursive: a listener may unsubscribe itself while it is called.
        std::recursive_mutex mutex;
        bool active = true;
        Listener listener;
    };
    struct State {
        std::mutex mutex;
        std::vector<std::shared_ptr<Slot>> slots;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace fem
