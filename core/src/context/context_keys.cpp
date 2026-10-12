#include "fem/core/context/context_keys.h"

#include "fem/core/context/error.h"
#include "fem/core/model/object.h"

#include "../model/utf8.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace fem::context {

struct ContextKeys::Watcher {
    WhenExpression expression;
    std::function<void(bool)> listener;
    bool value;          // Last evaluation.
    bool delivered;      // Last value given to the listener, or the first evaluation.
    bool active = true;  // False once the subscription is reset.
};

struct ContextKeys::Watchers {
    std::map<std::uint64_t, std::shared_ptr<Watcher>> by_id;  // Ids grow: creation order.
    std::map<std::string, std::set<std::uint64_t>, std::less<>> by_key;
    std::uint64_t next_id = 1;
};

namespace {

using model::detail::is_valid_utf8;

[[noreturn]] void fail(ContextErrc code, const std::string& message) {
    throw ContextError(code, message);
}

std::string in_quotes(std::string_view name) {
    return "'" + std::string(name) + "'";
}

/// Why `value` cannot be a value of a key of kind `type`; empty if it can.
std::string value_problem(ContextValue::Kind type, const ContextValue& value) {
    if (value.is_none()) {
        return {};
    }
    if (value.kind() != type) {
        return "a value of kind " + std::string(to_string(value.kind())) + " for a key of kind " +
               std::string(to_string(type));
    }
    switch (value.kind()) {
        case ContextValue::Kind::number:
            if (!std::isfinite(value.as_number())) {
                return "a number that is not finite";
            }
            break;
        case ContextValue::Kind::string:
            if (!is_valid_utf8(value.as_string())) {
                return "text that is not valid UTF-8";
            }
            break;
        case ContextValue::Kind::list: {
            const ContextValue::List& items = value.as_list();
            if (!std::all_of(items.begin(), items.end(),
                             [](const std::string& item) { return is_valid_utf8(item); })) {
                return "a list with text that is not valid UTF-8";
            }
            break;
        }
        default:
            break;
    }
    return {};
}

}  // namespace

ContextKeys::ContextKeys() : watchers_(std::make_shared<Watchers>()) {}

ContextKeys::~ContextKeys() = default;

// --- Declarations --------------------------------------------------------------------------------

bool ContextKeys::declare(KeySpec spec) {
    if (!is_valid_key(spec.name)) {
        fail(ContextErrc::invalid_key,
             in_quotes(spec.name) +
                 " is not a valid context key: it must be names of letters, digits, '_' and '-' "
                 "joined by single dots, start with a letter or '_' and not be true, false or in");
    }
    const std::string key = "context key " + in_quotes(spec.name);
    if (spec.owner.empty()) {
        fail(ContextErrc::invalid_spec, key + " has no owner");
    }
    if (spec.type == ContextValue::Kind::none) {
        fail(ContextErrc::invalid_spec, key + " has no type");
    }
    if (const std::string problem = value_problem(spec.type, spec.initial); !problem.empty()) {
        fail(ContextErrc::invalid_spec, key + " has a bad initial value: " + problem);
    }
    if (const auto it = entries_.find(spec.name); it != entries_.end()) {
        if (it->second.spec == spec) {
            return false;
        }
        fail(
            ContextErrc::duplicate_key,
            key + " is already declared by " + in_quotes(it->second.spec.owner) +
                (it->second.spec.owner == spec.owner ? " with another type or initial value" : ""));
    }
    std::string name = spec.name;
    ContextValue initial = spec.initial;
    const bool has_value = !initial.is_none();
    entries_.emplace(name, Entry{std::move(spec), std::move(initial)});
    if (has_value) {
        changed({name});
    }
    return true;
}

std::size_t ContextKeys::remove_owner(std::string_view owner) {
    if (owner == model::core_owner) {
        fail(ContextErrc::invalid_spec, "context keys of the core cannot be removed");
    }
    std::vector<std::string> with_values;
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.spec.owner != owner) {
            ++it;
            continue;
        }
        if (!it->second.value.is_none()) {
            with_values.push_back(it->first);
        }
        it = entries_.erase(it);
        ++removed;
    }
    changed(with_values);
    return removed;
}

const KeySpec* ContextKeys::find(std::string_view name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second.spec;
}

std::vector<std::string> ContextKeys::keys() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        names.push_back(name);
    }
    return names;
}

// --- Values --------------------------------------------------------------------------------------

const ContextValue& ContextKeys::value(std::string_view name) const {
    static const ContextValue none;
    const auto it = entries_.find(name);
    return it == entries_.end() ? none : it->second.value;
}

ContextKeys::Entry& ContextKeys::checked_entry(std::string_view name, const ContextValue& value) {
    const auto it = entries_.find(name);
    if (it == entries_.end()) {
        fail(ContextErrc::unknown_key, "context key " + in_quotes(name) + " is not declared");
    }
    if (const std::string problem = value_problem(it->second.spec.type, value); !problem.empty()) {
        fail(ContextErrc::invalid_value,
             "context key " + in_quotes(name) + " cannot take " + problem);
    }
    return it->second;
}

void ContextKeys::set(std::string_view name, ContextValue value) {
    Entry& entry = checked_entry(name, value);
    if (entry.value == value) {
        return;
    }
    entry.value = std::move(value);
    changed({entry.spec.name});
}

void ContextKeys::set(std::vector<std::pair<std::string, ContextValue>> values) {
    std::vector<Entry*> entries;
    entries.reserve(values.size());
    for (const auto& [name, value] : values) {
        entries.push_back(&checked_entry(name, value));
    }
    std::vector<std::string> names;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (entries[i]->value != values[i].second) {
            entries[i]->value = std::move(values[i].second);
            names.push_back(std::move(values[i].first));
        }
    }
    changed(names);
}

// --- Expressions ---------------------------------------------------------------------------------

bool ContextKeys::evaluate(const WhenExpression& expression) const {
    return expression.evaluate(
        [this](std::string_view name) -> const ContextValue& { return value(name); });
}

Subscription ContextKeys::watch(WhenExpression expression, std::function<void(bool)> listener) {
    if (!listener) {
        fail(ContextErrc::invalid_spec, "the listener of a watched expression is empty");
    }
    const bool value = evaluate(expression);
    const std::uint64_t id = watchers_->next_id++;
    auto watcher = std::make_shared<Watcher>(
        Watcher{std::move(expression), std::move(listener), value, value});
    for (const std::string& key : watcher->expression.keys()) {
        watchers_->by_key[key].insert(id);
    }
    watchers_->by_id.emplace(id, std::move(watcher));
    return Subscription([weak_watchers = std::weak_ptr<Watchers>(watchers_), id] {
        const std::shared_ptr<Watchers> watchers = weak_watchers.lock();
        if (!watchers) {
            return;
        }
        const auto it = watchers->by_id.find(id);
        if (it == watchers->by_id.end()) {
            return;
        }
        it->second->active = false;
        for (const std::string& key : it->second->expression.keys()) {
            const auto dependents = watchers->by_key.find(key);
            if (dependents == watchers->by_key.end()) {
                continue;
            }
            dependents->second.erase(id);
            if (dependents->second.empty()) {
                watchers->by_key.erase(dependents);
            }
        }
        watchers->by_id.erase(it);
    });
}

void ContextKeys::changed(const std::vector<std::string>& names) {
    std::set<std::uint64_t> ids;
    for (const std::string& name : names) {
        if (const auto it = watchers_->by_key.find(name); it != watchers_->by_key.end()) {
            ids.insert(it->second.begin(), it->second.end());
        }
    }
    // Evaluate first, then call listeners: a listener may change keys and watchers, and its
    // changes are evaluated right away (by a nested call), so each watcher is notified below only
    // if its latest value still differs from what its listener has seen.
    std::vector<std::shared_ptr<Watcher>> pending;
    for (const std::uint64_t id : ids) {
        const std::shared_ptr<Watcher>& watcher = watchers_->by_id.at(id);
        watcher->value = evaluate(watcher->expression);
        ++watch_evaluations_;
        if (watcher->value != watcher->delivered) {
            pending.push_back(watcher);
        }
    }
    for (const std::shared_ptr<Watcher>& watcher : pending) {
        if (watcher->active && watcher->value != watcher->delivered) {
            watcher->delivered = watcher->value;
            watcher->listener(watcher->value);
        }
    }
}

}  // namespace fem::context
