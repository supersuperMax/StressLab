#include "fem/core/command/command_bus.h"

#include "fem/core/command/error.h"
#include "fem/core/command/python_literal.h"
#include "fem/core/model/object.h"

#include "../model/utf8.h"
#include "model_commands_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <utility>

namespace fem::command {

namespace {

[[noreturn]] void fail(CommandErrc code, const std::string& message) {
    throw CommandError(code, message);
}

bool is_ascii_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

// Keywords of Python 3.12 that cannot be keyword arguments (soft keywords can).
constexpr std::array<std::string_view, 32> python_keywords = {
    "and",    "as",   "assert", "async",  "await",  "break",   "class",    "continue",
    "def",    "del",  "elif",   "else",   "except", "finally", "for",      "from",
    "global", "if",   "import", "in",     "is",     "lambda",  "nonlocal", "not",
    "or",     "pass", "raise",  "return", "try",    "while",   "with",     "yield",
};

/// `value` as an argument of type `type`, or nothing if it does not fit.
std::optional<Value> normalize(ArgType type, Value value) {
    const Value::Kind kind = value.kind();
    switch (type) {
        case ArgType::any:
            return value;
        case ArgType::boolean:
            return kind == Value::Kind::boolean ? std::optional<Value>(std::move(value))
                                                : std::nullopt;
        case ArgType::integer:
            return kind == Value::Kind::integer ? std::optional<Value>(std::move(value))
                                                : std::nullopt;
        case ArgType::number:
            if (kind == Value::Kind::integer) {
                return Value(value.as_number());
            }
            return kind == Value::Kind::number ? std::optional<Value>(std::move(value))
                                               : std::nullopt;
        case ArgType::string:
            return kind == Value::Kind::string ? std::optional<Value>(std::move(value))
                                               : std::nullopt;
        case ArgType::path:
            if (kind == Value::Kind::string) {
                const std::string& text = value.as_string();
                if (!model::detail::is_valid_utf8(text)) {
                    return std::nullopt;
                }
                return Value(std::filesystem::path(std::u8string(text.begin(), text.end())));
            }
            return kind == Value::Kind::path ? std::optional<Value>(std::move(value))
                                             : std::nullopt;
        case ArgType::object:
            if (kind == Value::Kind::integer && value.as_integer() >= 1) {
                return value;
            }
            return std::nullopt;
        case ArgType::list:
            return kind == Value::Kind::list ? std::optional<Value>(std::move(value))
                                             : std::nullopt;
        case ArgType::dict:
            return kind == Value::Kind::dict ? std::optional<Value>(std::move(value))
                                             : std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(ModelChange::Cause cause) noexcept {
    switch (cause) {
        case ModelChange::Cause::execute:
            return "execute";
        case ModelChange::Cause::undo:
            return "undo";
        case ModelChange::Cause::redo:
            return "redo";
        case ModelChange::Cause::load:
            return "load";
    }
    return "execute";
}

// --- Specs ---------------------------------------------------------------------------------------

bool is_valid_command_id(std::string_view id) noexcept {
    std::size_t segments = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = id.find('.', start);
        const std::string_view segment =
            id.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment.empty() || !is_ascii_letter(segment[0])) {
            return false;
        }
        for (const char c : segment) {
            if (!is_ascii_letter(c) && !is_ascii_digit(c) && c != '_' && c != '-') {
                return false;
            }
        }
        ++segments;
        if (end == std::string_view::npos) {
            return segments >= 2;
        }
        start = end + 1;
    }
}

bool is_valid_argument_name(std::string_view name) noexcept {
    if (name.empty() || is_ascii_digit(name[0])) {
        return false;
    }
    for (const char c : name) {
        if (!(c >= 'a' && c <= 'z') && !is_ascii_digit(c) && c != '_') {
            return false;
        }
    }
    return std::find(python_keywords.begin(), python_keywords.end(), name) == python_keywords.end();
}

void check_spec(CommandSpec& spec) {
    if (!is_valid_command_id(spec.id)) {
        fail(CommandErrc::invalid_spec, "invalid command id '" + spec.id + "'");
    }
    if (spec.owner.empty()) {
        fail(CommandErrc::invalid_spec, "command '" + spec.id + "' has no owner");
    }
    std::set<std::string_view> names;
    for (ArgSpec& arg : spec.args) {
        const std::string where = "argument '" + arg.name + "' of command '" + spec.id + "'";
        if (!is_valid_argument_name(arg.name)) {
            fail(CommandErrc::invalid_spec, "invalid name of " + where);
        }
        if (!names.insert(arg.name).second) {
            fail(CommandErrc::invalid_spec, "duplicate " + where);
        }
        if (arg.default_value) {
            if (arg.required) {
                fail(CommandErrc::invalid_spec, "required " + where + " has a default");
            }
            std::optional<Value> normalized = normalize(arg.type, *arg.default_value);
            if (!normalized) {
                fail(CommandErrc::invalid_spec, "the default of " + where + " is not of type " +
                                                    std::string(to_string(arg.type)));
            }
            arg.default_value = std::move(normalized);
        }
    }
}

// --- Transaction ---------------------------------------------------------------------------------

Transaction::Transaction(Transaction&& other) noexcept
    : bus_(std::exchange(other.bus_, nullptr)), id_(other.id_) {}

Transaction::~Transaction() {
    if (bus_ != nullptr) {
        bus_->rollback(id_);
    }
}

void Transaction::commit() {
    if (bus_ == nullptr) {
        fail(CommandErrc::invalid_state, "the transaction was moved away");
    }
    bus_->commit(id_);
}

void Transaction::rollback() {
    if (bus_ != nullptr) {
        bus_->rollback(id_);
    }
}

bool Transaction::is_open() const noexcept {
    return bus_ != nullptr && bus_->is_open(id_);
}

// --- CommandContext ------------------------------------------------------------------------------

const model::Document& CommandContext::document() const noexcept {
    return bus_.document_;
}

model::DocumentEditor& CommandContext::editor() const noexcept {
    return bus_.editor_;
}

CommandResult CommandContext::execute(std::string_view id, Value::Dict args) const {
    return bus_.execute(id, std::move(args));
}

// --- Registry ------------------------------------------------------------------------------------

CommandBus::CommandBus(model::Document& document) : document_(document), editor_(document) {
    detail::register_model_commands(*this);
}

CommandBus::~CommandBus() = default;

void CommandBus::register_command(CommandSpec spec, CommandHandler handler) {
    check_spec(spec);
    if (!handler) {
        fail(CommandErrc::invalid_spec, "command '" + spec.id + "' has no handler");
    }
    if (commands_.contains(spec.id)) {
        fail(CommandErrc::duplicate_command, "command '" + spec.id + "' is already registered");
    }
    std::string id = spec.id;
    commands_.emplace(std::move(id), std::make_shared<const Registered>(
                                         Registered{std::move(spec), std::move(handler)}));
}

bool CommandBus::unregister_command(std::string_view id) {
    const auto it = commands_.find(id);
    if (it == commands_.end()) {
        return false;
    }
    if (it->second->spec.owner == model::core_owner) {
        fail(CommandErrc::invalid_spec, "commands of the core cannot be unregistered");
    }
    commands_.erase(it);
    return true;
}

std::size_t CommandBus::unregister_owner(std::string_view owner) {
    if (owner == model::core_owner) {
        fail(CommandErrc::invalid_spec, "commands of the core cannot be unregistered");
    }
    return std::erase_if(commands_,
                         [owner](const auto& entry) { return entry.second->spec.owner == owner; });
}

const CommandSpec* CommandBus::find_command(std::string_view id) const {
    const auto it = commands_.find(id);
    return it == commands_.end() ? nullptr : &it->second->spec;
}

std::vector<std::string> CommandBus::command_ids() const {
    std::vector<std::string> ids;
    ids.reserve(commands_.size());
    for (const auto& [id, command] : commands_) {
        ids.push_back(id);
    }
    return ids;
}

std::shared_ptr<const CommandBus::Registered> CommandBus::lookup(std::string_view id) const {
    const auto it = commands_.find(id);
    if (it == commands_.end()) {
        fail(CommandErrc::unknown_command, "no command '" + std::string(id) + "'");
    }
    return it->second;
}

// --- Running -------------------------------------------------------------------------------------

Arguments CommandBus::check_arguments(const CommandSpec& spec, Value::Dict args,
                                      Value::Dict& given) const {
    Value::Dict values;
    for (auto& [name, value] : args) {
        const ArgSpec* arg = spec.find_arg(name);
        if (arg == nullptr) {
            fail(CommandErrc::invalid_arguments,
                 "command '" + spec.id + "' has no argument '" + name + "'");
        }
        if (value.is_none() && !arg->required) {
            continue;  // None stands for an omitted optional argument, as in Python.
        }
        const Value::Kind kind = value.kind();
        std::optional<Value> normalized = normalize(arg->type, std::move(value));
        if (!normalized) {
            fail(CommandErrc::invalid_arguments,
                 "argument '" + name + "' of command '" + spec.id + "' must be " +
                     std::string(to_string(arg->type)) + ", not " + std::string(to_string(kind)));
        }
        given.emplace(name, *normalized);
        values.emplace(name, std::move(*normalized));
    }
    for (const ArgSpec& arg : spec.args) {
        if (values.contains(arg.name)) {
            continue;
        }
        if (arg.required) {
            fail(CommandErrc::invalid_arguments,
                 "command '" + spec.id + "' needs argument '" + arg.name + "'");
        }
        if (arg.default_value) {
            values.emplace(arg.name, *arg.default_value);
        }
    }
    return Arguments(std::move(values));
}

std::string CommandBus::format_call(const CommandSpec& spec, const Value::Dict& given) {
    std::string line = "stresslab.cmd(" + python_string(spec.id);
    for (const ArgSpec& arg : spec.args) {
        const auto it = given.find(arg.name);
        if (it == given.end()) {
            continue;
        }
        line += ", ";
        line += arg.name;
        line += '=';
        line += to_python(it->second);
    }
    line += ')';
    return line;
}

std::string CommandBus::format_call(std::string_view id, const Value::Dict& args) const {
    const std::shared_ptr<const Registered> command = lookup(id);
    Value::Dict given;
    (void)check_arguments(command->spec, args, given);
    return format_call(command->spec, given);
}

CommandResult CommandBus::execute(std::string_view id, Value::Dict args) {
    const std::shared_ptr<const Registered> command = lookup(id);
    Value::Dict given;
    const Arguments arguments = check_arguments(command->spec, std::move(args), given);

    if (running_ > 0) {
        // Run by a running command: part of its step, and only that command is journaled.
        model::ChangeSet* parent = editor_.recording();
        model::ChangeSet changes = editor_.new_changes();
        CommandResult result = run(command, arguments, changes);
        parent->append(std::move(changes));
        return result;
    }

    // Formatting checks that the journal can hold the arguments, before anything changes.
    std::string line = format_call(command->spec, given);
    model::ChangeSet changes = editor_.new_changes();
    CommandResult result = run(command, arguments, changes);
    if (!transactions_.empty()) {
        OpenTransaction& transaction = transactions_.back();
        transaction.changes.append(std::move(changes));
        transaction.lines.push_back(std::move(line));
        return result;
    }
    complete_step(command->spec.id, std::move(changes), line);
    return result;
}

CommandResult CommandBus::run(const std::shared_ptr<const Registered>& command,
                              const Arguments& args, model::ChangeSet& changes) {
    model::ChangeSet* const previous = editor_.record_into(&changes);
    ++running_;
    try {
        CommandContext context(*this, command->spec, args);
        CommandResult result = command->handler(context);
        --running_;
        editor_.record_into(previous);
        return result;
    } catch (...) {
        --running_;
        editor_.record_into(previous);
        editor_.rollback(changes);
        throw;
    }
}

void CommandBus::complete_step(std::string label, model::ChangeSet changes,
                               const std::string& entry) {
    if (!entry.empty()) {
        journal_.emit(entry);
    }
    if (changes.empty()) {
        return;
    }
    ModelChange change{ModelChange::Cause::execute, label, changes.summary()};
    undo_stack_.push_back(Step{std::move(label), std::move(changes)});
    redo_stack_.clear();
    trim_undo_stack();
    model_changed_.emit(change);
}

// --- History -------------------------------------------------------------------------------------

std::string_view CommandBus::undo_label() const noexcept {
    return undo_stack_.empty() ? std::string_view() : std::string_view(undo_stack_.back().label);
}

std::string_view CommandBus::redo_label() const noexcept {
    return redo_stack_.empty() ? std::string_view() : std::string_view(redo_stack_.back().label);
}

void CommandBus::check_idle(std::string_view action) const {
    if (running_ > 0) {
        fail(CommandErrc::invalid_state, std::string(action) + " is not allowed inside a command");
    }
    if (!transactions_.empty()) {
        fail(CommandErrc::invalid_state,
             std::string(action) + " is not allowed inside a transaction");
    }
}

void CommandBus::journal_action(std::string_view entry) {
    check_idle("an action of the session");
    if (entry.empty()) {
        fail(CommandErrc::invalid_arguments, "an action of the session needs a journal line");
    }
    journal_.emit(entry);
}

void CommandBus::replace_document(model::DocumentContents contents, std::string_view entry) {
    check_idle("replacing the document");
    std::set<model::ObjectId> before;
    for (const model::Object& object : document_.objects()) {
        before.insert(object.id());
    }
    const model::UnitSystem units = document_.units();
    editor_.replace(std::move(contents));

    ModelChange change{ModelChange::Cause::load, {}, {}};
    for (const model::Object& object : document_.objects()) {
        (before.erase(object.id()) > 0 ? change.summary.updated : change.summary.created)
            .push_back(object.id());
    }
    change.summary.removed.assign(before.begin(), before.end());
    change.summary.units = units != document_.units();
    undo_stack_.clear();
    redo_stack_.clear();
    if (!entry.empty()) {
        journal_.emit(entry);
    }
    model_changed_.emit(change);
}

void CommandBus::set_undo_limit(std::size_t limit) {
    undo_limit_ = limit;
    trim_undo_stack();
}

void CommandBus::trim_undo_stack() {
    if (undo_limit_ > 0 && undo_stack_.size() > undo_limit_) {
        const auto excess = static_cast<std::ptrdiff_t>(undo_stack_.size() - undo_limit_);
        undo_stack_.erase(undo_stack_.begin(), undo_stack_.begin() + excess);
    }
}

bool CommandBus::undo() {
    check_idle("undo");
    if (undo_stack_.empty()) {
        return false;
    }
    Step& step = undo_stack_.back();
    editor_.undo(step.changes);
    const ModelChange change{ModelChange::Cause::undo, step.label, step.changes.summary()};
    redo_stack_.push_back(std::move(step));
    undo_stack_.pop_back();
    journal_.emit("stresslab.undo()");
    model_changed_.emit(change);
    return true;
}

bool CommandBus::redo() {
    check_idle("redo");
    if (redo_stack_.empty()) {
        return false;
    }
    Step& step = redo_stack_.back();
    editor_.redo(step.changes);
    const ModelChange change{ModelChange::Cause::redo, step.label, step.changes.summary()};
    undo_stack_.push_back(std::move(step));
    redo_stack_.pop_back();
    journal_.emit("stresslab.redo()");
    model_changed_.emit(change);
    return true;
}

// --- Transactions --------------------------------------------------------------------------------

Transaction CommandBus::begin_transaction(std::string label) {
    if (running_ > 0) {
        fail(CommandErrc::invalid_state, "a transaction cannot start inside a command");
    }
    (void)python_string(label);  // The journal must be able to hold the label.
    const std::uint64_t id = next_transaction_id_++;
    transactions_.push_back(OpenTransaction{id, std::move(label), editor_.new_changes(), {}});
    return Transaction(*this, id);
}

bool CommandBus::is_open(std::uint64_t id) const noexcept {
    return std::any_of(transactions_.begin(), transactions_.end(),
                       [id](const OpenTransaction& transaction) { return transaction.id == id; });
}

void CommandBus::commit(std::uint64_t id) {
    if (!is_open(id)) {
        fail(CommandErrc::invalid_state, "the transaction is not open");
    }
    if (transactions_.back().id != id) {
        fail(CommandErrc::transaction_order, "a transaction inside this one is still open");
    }
    if (running_ > 0) {
        fail(CommandErrc::invalid_state, "a transaction cannot end inside a command");
    }
    OpenTransaction transaction = std::move(transactions_.back());
    transactions_.pop_back();

    if (!transactions_.empty()) {
        OpenTransaction& outer = transactions_.back();
        outer.changes.append(std::move(transaction.changes));
        outer.lines.insert(outer.lines.end(), std::make_move_iterator(transaction.lines.begin()),
                           std::make_move_iterator(transaction.lines.end()));
        return;
    }

    std::string entry;
    if (!transaction.lines.empty()) {
        entry = "with stresslab.transaction(" +
                (transaction.label.empty() ? std::string() : python_string(transaction.label)) +
                "):";
        for (const std::string& line : transaction.lines) {
            entry += "\n    ";
            entry += line;
        }
    }
    complete_step(std::move(transaction.label), std::move(transaction.changes), entry);
}

void CommandBus::rollback(std::uint64_t id) {
    if (!is_open(id)) {
        return;
    }
    if (running_ > 0) {
        fail(CommandErrc::invalid_state, "a transaction cannot end inside a command");
    }
    // Innermost first: their changes are the latest.
    while (true) {
        OpenTransaction transaction = std::move(transactions_.back());
        transactions_.pop_back();
        editor_.rollback(transaction.changes);
        if (transaction.id == id) {
            break;
        }
    }
}

// --- Events --------------------------------------------------------------------------------------

Subscription CommandBus::on_model_changed(std::function<void(const ModelChange&)> listener) {
    return model_changed_.subscribe(std::move(listener));
}

Subscription CommandBus::on_journal(std::function<void(std::string_view entry)> listener) {
    return journal_.subscribe(std::move(listener));
}

}  // namespace fem::command
