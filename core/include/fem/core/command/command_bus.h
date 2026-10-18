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
    /// `load`: the document got other contents (`CommandBus::replace_document`), such as an
    /// opened project; listeners should read the whole model again.
    enum class Cause : std::uint8_t { execute, undo, redo, load };

    Cause cause = Cause::execute;
    /// Id of the command, or label of the transaction.
    std::string label;
    model::ChangeSummary summary;
};

/// "execute", "undo", "redo", "load": the cause in Python and in the protocol of modules.
[[nodiscard]] std::string_view to_string(ModelChange::Cause cause) noexcept;

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

    /// Makes the document read-only, as while the window shows a version of the history (task
    /// 2.6): a command that changed the model is reverted and throws CommandError(read_only),
    /// inside a transaction too, and so do undo and redo. Commands that change nothing still run,
    /// and `replace_document` (opening a project or another version) is allowed.
    void set_read_only(bool read_only) noexcept { read_only_ = read_only; }
    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }

    /// Starts a group of commands that is undone as one step. Transactions nest; while one is
    /// open, undo and redo are refused and nothing is sent to listeners. The label names the
    /// step for undo and in the journal. Throws CommandError(invalid_state) inside a command.
    [[nodiscard]] Transaction begin_transaction(std::string label = {});
    [[nodiscard]] bool in_transaction() const noexcept { return !transactions_.empty(); }
    /// No command is running and no transaction is open: what runs now is a step of its own, not
    /// a part of another one. Work that waits for the model (requests of out-of-process modules)
    /// is done at such points.
    [[nodiscard]] bool is_idle() const noexcept { return running_ == 0 && transactions_.empty(); }

    /// Writes to the journal what changes the session but not the model, as Python that repeats
    /// it (`stresslab.extensions.enable("fem.demo")`): an entry of its own, with no undo step, and
    /// the steps to redo stay. Throws CommandError(invalid_state) inside a command or an open
    /// transaction, whose undo could not take the action back, and
    /// CommandError(invalid_arguments) for an empty entry.
    void journal_action(std::string_view entry);

    /// Makes the document hold `contents` (opening a project, task 2.1): not a step, so the steps
    /// to undo and redo, which belong to the old contents, are dropped. Writes `entry` to the
    /// journal if it is not empty, then tells the listeners with `ModelChange::Cause::load`, whose
    /// summary lists the objects of the new contents as created, or as updated if an object with
    /// the same id was there before. Throws ModelError and changes nothing for invalid contents
    /// (`Document` checks them), CommandError(invalid_state) inside a command or a transaction.
    void replace_document(model::DocumentContents contents, std::string_view entry = {});

    // --- Events ----------------------------------------------------------------------------------

    /// Called after every completed step that changed the model: a command outside transactions,
    /// a committed outermost transaction, undo and redo. Listeners may run commands.
    [[nodiscard]] Subscription on_model_changed(std::function<void(const ModelChange&)> listener);
    /// Called with every completed journal entry: one line, or several lines for a transaction,
    /// without the final line break. Called before the "model changed" listeners.
    [[nodiscard]] Subscription on_journal(std::function<void(std::string_view entry)> listener);
    /// Called before a command runs as a step of its own (not inside another command or a
    /// transaction), after its arguments are checked: the bus is idle, so a listener may write to
    /// the journal or save the project. The history makes an automatic version here before a
    /// command with `CommandSpec::snapshot_before`. A listener that throws stops the command,
    /// which then does not run.
    [[nodiscard]] Subscription on_before_command(
        std::function<void(const CommandSpec& spec)> listener);
    /// Called with every problem a running command reports (`CommandContext::report`), at once,
    /// while the command runs: listeners must not run commands or change the model.
    [[nodiscard]] Subscription on_problem(std::function<void(const Problem& problem)> listener);

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
    /// While read-only, reverts `changes` of the command `id` and throws if there are any.
    void refuse_changes(model::ChangeSet& changes, std::string_view id);
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
    bool read_only_ = false;
    std::vector<OpenTransaction> transactions_;
    std::uint64_t next_transaction_id_ = 1;
    int running_ = 0;  // Depth of commands being run.
    Signal<const ModelChange&> model_changed_;
    Signal<std::string_view> journal_;
    Signal<const CommandSpec&> before_command_;
    Signal<const Problem&> problem_;
};

}  // namespace fem::command
