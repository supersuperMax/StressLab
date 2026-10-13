#pragma once

#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fem::app {

/// What `stresslab --headless ... run-script <file> [args]` runs (docs/architecture.md §4.7).
struct HeadlessOptions {
    /// `--extensions`: folders whose subfolders are modules to install, in this order.
    std::vector<std::filesystem::path> extension_folders;
    /// `--profile`: the modules to enable; without it every installed module is enabled.
    std::optional<std::filesystem::path> profile;
    /// `--journal`: where the journal of the session (.slpy) is written.
    std::optional<std::filesystem::path> journal;
    /// `--log-level`; without it the setting "log.level" decides.
    std::optional<log::Level> log_level;
    /// `--log-file`: also write the log to this file.
    std::optional<std::filesystem::path> log_file;
    std::filesystem::path script;
    /// `sys.argv[1:]` of the script.
    std::vector<std::string> script_args;
};

struct CommandLine {
    enum class Mode : std::uint8_t {
        window,    ///< No arguments: the window (a build without the GUI prints the usage)
        help,      ///< --help, -h
        version,   ///< --version
        python,    ///< --python [arguments of python]
        headless,  ///< --headless [options] run-script <file> [arguments]
    };

    Mode mode = Mode::window;
    /// For `python`: the arguments after --python.
    std::vector<std::string> python_args;
    /// For `headless`.
    HeadlessOptions headless;
};

/// A command line that cannot be run. The message for the user is the string `key` of the core
/// with `args`; `what()` is the same in English, for logs and tests.
class CommandLineError : public std::runtime_error {
public:
    CommandLineError(std::string key, l10n::Args args, const std::string& what)
        : std::runtime_error(what), key_(std::move(key)), args_(std::move(args)) {}

    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const l10n::Args& args() const noexcept { return args_; }

private:
    std::string key_;
    l10n::Args args_;
};

/// Parses the arguments of `stresslab` (without the program name; UTF-8). Options of
/// `--headless` may stand before and after `run-script`, up to the name of the script; `--` ends
/// them. Everything after the name of the script goes to the script. Throws CommandLineError.
[[nodiscard]] CommandLine parse_command_line(const std::vector<std::string>& args);

/// A path from a UTF-8 argument.
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& text);

}  // namespace fem::app
