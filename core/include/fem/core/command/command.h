#pragma once

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

    [[nodiscard]] const ArgSpec* find_arg(std::string_view name) const;
};

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
