#pragma once

#include "fem/core/command/command.h"
#include "fem/core/command/value.h"
#include "fem/core/model/change_set.h"
#include "fem/core/model/document.h"
#include "fem/core/model/document_editor.h"
#include "fem/core/signal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fem::command {

/// "The model changed": sent once for every completed step that changed it.
struct ModelChange {
    enum class Cause : std::uint8_t { execute, undo, redo };

    Cause cause = Cause::execute;
    /// Id of the command, or label of the transaction.
    std::string label;
    model::ChangeSummary summary;
};

/// A group of commands that is undone as one step (see `CommandBus::begin_transaction`).
/// Rolls back on destruction unless committed. Must not outlive its bus.
class Transaction {
public:
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    /// Ends the transaction. The outermost one becomes one undo step and one journal entry;
    /// an inner one joins the transaction around it. Throws CommandError(transaction_order) while a
    /// transaction inside this one is open, CommandError(invalid_state) if it is not open.
    void commit();
    /// Reverts every change made since `begin_transaction` (and closes transactions opened inside
    /// this one); nothing reaches the journal. Does nothing if the transaction is not open.
    void rollback();
    [[nodiscard]] bool is_open() const noexcept;

private:
    friend class CommandBus;

    Transaction(CommandBus& bus, std::uint64_t id) noexcept : bus_(&bus), id_(id) {}

    CommandBus* bus_;
    std::uint64_t id_;
};

/// Runs commands: the only way to change a document (CLAUDE.md, rule 3).
///
/// A command has an id, an owner and a schema of keyword arguments (docs/architecture.md §4.2).
/// The bus checks the arguments, runs the command and records its changes, so that every *step*
/// can be undone and redone: a step is one command, or all commands of a transaction. Every
/// completed call is written to the journal as a line of Python, `stresslab.cmd("id", key=value)`
/// (see `on_journal`), together with undo, redo and transactions, so the journal replayed in a new
/// document gives the same document.
///
/// A command that throws leaves the model as it was, and nothing about it is journaled. Commands
/// that a command runs itself belong to its step and are not journaled. Single-threaded, like the
/// model.
class CommandBus {
public:
    /// The bus edits `document`, which must outlive it, and registers the commands of the core
    /// ("model.*", see `model_commands.h`).
    explicit CommandBus(model::Document& document);
    CommandBus(const CommandBus&) = delete;
    CommandBus& operator=(const CommandBus&) = delete;
    CommandBus(CommandBus&&) = delete;
    CommandBus& operator=(CommandBus&&) = delete;
    ~CommandBus();

    [[nodiscard]] const model::Document& document() const noexcept { return document_; }

    // --- Registry --------------------------------------------------------------------------------

    /// Throws CommandError(invalid_spec) for a bad spec or an empty handler and
    /// CommandError(duplicate_command) if the id is taken.
    void register_command(CommandSpec spec, CommandHandler handler);
    /// Returns false if there is no such command. Commands of the core cannot be unregistered.
    bool unregister_command(std::string_view id);
    /// Unregisters every command of a module (when it is disabled) and returns how many.
    std::size_t unregister_owner(std::string_view owner);
    [[nodiscard]] const CommandSpec* find_command(std::string_view id) const;
    /// Ids of all registered commands, sorted.
    [[nodiscard]] std::vector<std::string> command_ids() const;

    // --- Running ---------------------------------------------------------------------------------

    /// Runs a command. Throws CommandError(unknown_command, invalid_arguments, invalid_value)
    /// before anything runs; whatever the command throws is passed on after its changes are
    /// reverted.
    CommandResult execute(std::string_view id, Value::Dict args = {});
    /// The journal line of a call, after the arguments are checked as `execute` checks them.
    [[nodiscard]] std::string format_call(std::string_view id, const Value::Dict& args) const;

    // --- History ---------------------------------------------------------------------------------

    [[nodiscard]] bool can_undo() const noexcept { return !undo_stack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_stack_.empty(); }
    /// Label of the step `undo` would revert (command id or transaction label); empty if none.
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;

    /// Reverts the last step; returns false if there is none. Throws ModelError(opaque_object)
    /// and changes nothing if the step touches an object of a module disabled since then.
    /// Throws CommandError(invalid_state) inside a command or an open transaction.
    bool undo();
    /// Repeats the last undone step; returns false if there is none. Any new step clears the
    /// steps to redo.
    bool redo();

    /// How many steps can be undone; 0 (the default) means no limit. When a new step goes over
    /// the limit, the oldest one is forgotten; lowering the limit forgets the oldest steps at once.
    /// Steps to redo are not limited. The setting "history.undoLimit" drives it
    /// (`fem::bind_undo_limit`).
    void set_undo_limit(std::size_t limit);
    [[nodiscard]] std::size_t undo_limit() const noexcept { return undo_limit_; }
    /// How many steps can be undone now.
    [[nodiscard]] std::size_t undo_count() const noexcept { return undo_stack_.size(); }

    /// Starts a group of commands that is undone as one step. Transactions nest; while one is
    /// open, undo and redo are refused and nothing is sent to listeners. The label names the
    /// step for undo and in the journal. Throws CommandError(invalid_state) inside a command.
    [[nodiscard]] Transaction begin_transaction(std::string label = {});
    [[nodiscard]] bool in_transaction() const noexcept { return !transactions_.empty(); }

    // --- Events ----------------------------------------------------------------------------------

    /// Called after every completed step that changed the model: a command outside transactions,
    /// a committed outermost transaction, undo and redo. Listeners may run commands.
    [[nodiscard]] Subscription on_model_changed(std::function<void(const ModelChange&)> listener);
    /// Called with every completed journal entry: one line, or several lines for a transaction,
    /// without the final line break. Called before the "model changed" listeners.
    [[nodiscard]] Subscription on_journal(std::function<void(std::string_view entry)> listener);

private:
    friend class CommandContext;
    friend class Transaction;

    struct Registered {
        CommandSpec spec;
        CommandHandler handler;
    };
    struct Step {
        std::string label;
        model::ChangeSet changes;
    };
    struct OpenTransaction {
        std::uint64_t id;
        std::string label;
        model::ChangeSet changes;
        std::vector<std::string> lines;  // Journal lines of the commands, without indentation.
    };

    /// The command; throws CommandError(unknown_command). Shared: a command may unregister itself.
    [[nodiscard]] std::shared_ptr<const Registered> lookup(std::string_view id) const;
    /// Checks and normalizes `args`; `given` receives what the caller passed, for the journal.
    [[nodiscard]] Arguments check_arguments(const CommandSpec& spec, Value::Dict args,
                                            Value::Dict& given) const;
    [[nodiscard]] static std::string format_call(const CommandSpec& spec, const Value::Dict& given);
    /// Runs a command, recording into `changes`; reverts them if the command throws.
    CommandResult run(const std::shared_ptr<const Registered>& command, const Arguments& args,
                      model::ChangeSet& changes);
    void complete_step(std::string label, model::ChangeSet changes, const std::string& entry);
    void check_idle(std::string_view action) const;
    /// Forgets the oldest steps over the undo limit.
    void trim_undo_stack();

    // For Transaction.
    void commit(std::uint64_t id);
    void rollback(std::uint64_t id);
    [[nodiscard]] bool is_open(std::uint64_t id) const noexcept;

    model::Document& document_;
    model::DocumentEditor editor_;
    std::map<std::string, std::shared_ptr<const Registered>, std::less<>> commands_;
    std::vector<Step> undo_stack_;
    std::vector<Step> redo_stack_;
    std::size_t undo_limit_ = 0;
    std::vector<OpenTransaction> transactions_;
    std::uint64_t next_transaction_id_ = 1;
    int running_ = 0;  // Depth of commands being run.
    Signal<const ModelChange&> model_changed_;
    Signal<std::string_view> journal_;
};

}  // namespace fem::command
