#pragma once

#include "fem/core/context/value.h"
#include "fem/core/context/when.h"
#include "fem/core/signal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::context {

/// Declaration of a context key.
struct KeySpec {
    /// Name of the key, e.g. "model.hasMesh"; see `is_valid_key`.
    std::string name{};
    /// Id of the module that declares the key, e.g. "fem.core".
    std::string owner{};
    /// Kind of the values of the key; not none.
    ContextValue::Kind type = ContextValue::Kind::boolean;
    /// Value right after the declaration: none or of kind `type`.
    ContextValue initial{};

    friend bool operator==(const KeySpec&, const KeySpec&) = default;
};

/// The dictionary of context keys: what `when` expressions of menus, toolbars and panels are
/// evaluated against (docs/architecture.md §4.3).
///
/// A key is declared by its owner (the core, the shell or a module) with the kind of its values,
/// and is removed with the owner when a module is disabled. A key that is not declared, or not
/// set, has no value; expressions may read such keys, so a module can show a button on a key of
/// another module that is not enabled yet.
///
/// `watch` reports every change of the value of an expression. When keys change, only the watched
/// expressions that read them are evaluated again, each once per change, however many of its keys
/// changed. Single-threaded, like the model.
class ContextKeys {
public:
    ContextKeys();
    ContextKeys(const ContextKeys&) = delete;
    ContextKeys& operator=(const ContextKeys&) = delete;
    ContextKeys(ContextKeys&&) = delete;
    ContextKeys& operator=(ContextKeys&&) = delete;
    ~ContextKeys();

    // --- Declarations ----------------------------------------------------------------------------

    /// Declares a key and gives it the initial value. Returns false and changes nothing if the key
    /// is already declared with the same spec. Throws ContextError(invalid_key) for a bad name,
    /// ContextError(invalid_spec) for an empty owner, a none type or a bad initial value, and
    /// ContextError(duplicate_key) if the key is declared with another spec.
    bool declare(KeySpec spec);
    /// Removes every key of a module (when it is disabled) with its value, and returns how many.
    /// Throws ContextError(invalid_spec) for the core: its keys stay while the application runs.
    std::size_t remove_owner(std::string_view owner);
    /// The declaration, or nullptr if the key is not declared.
    [[nodiscard]] const KeySpec* find(std::string_view name) const;
    /// Names of all declared keys, sorted.
    [[nodiscard]] std::vector<std::string> keys() const;

    // --- Values ----------------------------------------------------------------------------------

    /// The value of a key; none if it is not declared or not set.
    [[nodiscard]] const ContextValue& value(std::string_view name) const;
    /// Sets the value of a declared key; none clears it. Throws ContextError(unknown_key) if the
    /// key is not declared and ContextError(invalid_value) for a value of another kind, a
    /// non-finite number or text that is not valid UTF-8.
    void set(std::string_view name, ContextValue value);
    /// Sets several keys at once: watched expressions are evaluated once, after all of them, so
    /// listeners never see a mix of old and new values. Checks every value before setting any;
    /// throws as `set` does. If a key comes twice, the last value wins.
    void set(std::vector<std::pair<std::string, ContextValue>> values);

    // --- Expressions -----------------------------------------------------------------------------

    /// The value of `expression` for the current values of the keys.
    [[nodiscard]] bool evaluate(const WhenExpression& expression) const;
    /// Calls `listener` with the new value every time the value of `expression` changes; the value
    /// now is `evaluate(expression)`. The listener may set keys and watch or stop watching
    /// expressions; it is never called twice in a row with the same value. An exception thrown by
    /// a listener propagates to the code that changed the keys, and the listeners after it are
    /// called on a later change. Throws ContextError(invalid_spec) for an empty listener.
    [[nodiscard]] Subscription watch(WhenExpression expression, std::function<void(bool)> listener);
    /// How many times watched expressions have been evaluated again after keys changed. For
    /// diagnostics and tests of incremental evaluation.
    [[nodiscard]] std::uint64_t watch_evaluations() const noexcept { return watch_evaluations_; }

private:
    struct Entry {
        KeySpec spec;
        ContextValue value;
    };
    struct Watcher;
    struct Watchers;

    /// The entry of a key that may take `value`; throws as `set` does.
    [[nodiscard]] Entry& checked_entry(std::string_view name, const ContextValue& value);
    /// Evaluates again the watched expressions that read `names`, then notifies listeners.
    void changed(const std::vector<std::string>& names);

    std::map<std::string, Entry, std::less<>> entries_;
    std::shared_ptr<Watchers> watchers_;
    std::uint64_t watch_evaluations_ = 0;
};

}  // namespace fem::context
