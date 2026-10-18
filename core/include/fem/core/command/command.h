#pragma once

#include "fem/core/command/problem.h"
#include "fem/core/command/value.h"
#include "fem/core/model/document.h"
#include "fem/core/model/document_editor.h"
#include "fem/core/model/object_id.h"
#include "fem/core/task/task.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fem::command {

class CommandBus;

/// Type of a command argument. The bus checks arguments against it before the command runs.
enum class ArgType : std::uint8_t {
    boolean,
    integer,
    number,  ///< Accepts integers too; the command and the journal see a number.
    string,
    path,    ///< Accepts strings too (UTF-8); the command and the journal see a path.
    object,  ///< Id of a model object: an integer from 1.
    list,
    dict,
    any,
};

/// "boolean", "integer", "number", "string", "path", "object", "list", "dict", "any".
[[nodiscard]] std::string_view to_string(ArgType type) noexcept;
/// The type named `name` by `to_string`, or nothing.
[[nodiscard]] std::optional<ArgType> parse_arg_type(std::string_view name) noexcept;

/// A keyword argument of a command.
struct ArgSpec {
    /// Python keyword: lowercase ASCII letters, digits and '_', not starting with a digit and not
    /// a Python keyword.
    std::string name{};
    ArgType type = ArgType::any;
    /// An optional argument may be omitted or passed as None.
    bool required = true;
    /// What the command sees when an optional argument is omitted; without it the argument is
    /// absent. Defaults are not written to the journal.
    std::optional<Value> default_value{};
};

/// Describes a command: its id, the module that owns it and its arguments.
struct CommandSpec {
    /// "module.command": two or more segments separated by '.', each starting with an ASCII
    /// letter and made of ASCII letters, digits, '_' and '-'.
    std::string id{};
    /// Id of the module that registered the command, e.g. "fem.core".
    std::string owner{};
    /// In this order the journal writes them.
    std::vector<ArgSpec> args{};
    /// A risky operation (rebuilding a mesh, an import over the model, running a solver): the
    /// project makes an automatic version before it runs (task 2.3,
    /// `CommandBus::on_before_command`). `snapshotBefore` in the manifest.
    bool snapshot_before = false;

    [[nodiscard]] const ArgSpec* find_arg(std::string_view name) const;
};

/// True if `id` can be the id of a command (see `CommandSpec::id`).
[[nodiscard]] bool is_valid_command_id(std::string_view id) noexcept;
/// True if `name` can be the name of an argument (see `ArgSpec::name`).
[[nodiscard]] bool is_valid_argument_name(std::string_view name) noexcept;
/// Checks a spec as `CommandBus::register_command` does and converts the defaults of its arguments
/// to their types (an integer default of a number argument becomes a number). Throws
/// CommandError(invalid_spec). Lets a module manifest be checked before its commands are
/// registered.
void check_spec(CommandSpec& spec);

/// Arguments as a command receives them: checked against its schema, with paths and numbers
/// normalized and defaults filled in. The typed accessors throw CommandError(invalid_arguments)
/// if the argument is absent or of another kind.
class Arguments {
public:
    Arguments() = default;
    explicit Arguments(Value::Dict values) : values_(std::move(values)) {}

    [[nodiscard]] bool has(std::string_view name) const;
    /// The argument, or nullptr if it is absent.
    [[nodiscard]] const Value* find(std::string_view name) const;
    [[nodiscard]] const Value& at(std::string_view name) const;

    [[nodiscard]] bool boolean(std::string_view name) const;
    [[nodiscard]] std::int64_t integer(std::string_view name) const;
    [[nodiscard]] double number(std::string_view name) const;
    [[nodiscard]] const std::string& string(std::string_view name) const;
    [[nodiscard]] const std::filesystem::path& path(std::string_view name) const;
    [[nodiscard]] model::ObjectId object(std::string_view name) const;
    /// A list of object ids.
    [[nodiscard]] std::vector<model::ObjectId> objects(std::string_view name) const;
    [[nodiscard]] const Value::List& list(std::string_view name) const;
    [[nodiscard]] const Value::Dict& dict(std::string_view name) const;

    [[nodiscard]] const Value::Dict& values() const noexcept { return values_; }

private:
    Value::Dict values_;
};

/// What a command returns: a value for the caller (in Python, the result of `stresslab.cmd`)
/// and, for a command that goes on in the background, its task.
struct CommandResult {
    CommandResult() = default;
    CommandResult(Value value) : value(std::move(value)) {}  // Lets handlers return a value.

    Value value;
    std::shared_ptr<task::Task> task;
};

/// What a running command can use. Valid only while the command runs.
class CommandContext {
public:
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    [[nodiscard]] const CommandSpec& spec() const noexcept { return spec_; }
    [[nodiscard]] const Arguments& args() const noexcept { return args_; }
    [[nodiscard]] const model::Document& document() const noexcept;
    /// Every change of the model goes through it and is recorded for undo.
    [[nodiscard]] model::DocumentEditor& editor() const noexcept;
    /// Runs another command as a part of this one: its changes join this command's undo step
    /// and only this command is written to the journal.
    CommandResult execute(std::string_view id, Value::Dict args = {}) const;
    /// Tells the listeners of `CommandBus::on_problem` at once, with the owner and id of this
    /// command filled in. Problems are not undone: they reach the user even if the command fails
    /// afterwards, which is how a failing import says where the file is wrong.
    void report(Problem problem) const;

private:
    friend class CommandBus;

    CommandContext(CommandBus& bus, const CommandSpec& spec, const Arguments& args) noexcept
        : bus_(bus), spec_(spec), args_(args) {}

    CommandBus& bus_;
    const CommandSpec& spec_;
    const Arguments& args_;
};

/// The code of a command. It changes the model only through `CommandContext::editor()`, must
/// give the same result for the same arguments and model (the journal is replayed), and may throw:
/// the bus then reverts what it changed.
using CommandHandler = std::function<CommandResult(CommandContext& context)>;

}  // namespace fem::command
