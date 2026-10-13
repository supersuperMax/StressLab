#pragma once

#include "command_line.h"

#include <filesystem>
#include <optional>

namespace fem::app {

/// What the headless run takes from its surroundings rather than from the command line.
struct HeadlessEnvironment {
    /// The executable of the application: it also runs the extension host of python modules
    /// (`stresslab --python -m stresslab_host`).
    std::filesystem::path executable;
    /// The folder of the modules shipped with the application; empty or missing: none.
    std::filesystem::path builtin_extensions;
    /// The settings file of the user, if known; a missing file is no error.
    std::optional<std::filesystem::path> user_settings;
};

/// Exit codes of `stresslab --headless` besides those of the script (`sys.exit(n)`).
inline constexpr int exit_success = 0;
/// The script raised, or the application failed (a journal that could not be written).
inline constexpr int exit_failure = 1;
/// A wrong command line: an unknown option, a script or a folder that does not exist.
inline constexpr int exit_usage = 2;

/// Runs a script without the window (task 1.10, ADR 0011): the document of the application with
/// its Command Bus and context keys, the settings of the user, the modules (built-in, from
/// `--extensions`, enabled all or by `--profile`, python and process modules in their own
/// processes), the embedded Python attached to the document. The log goes to the standard
/// output (and `--log-file`); the script's own output goes there too, line by line. Returns the
/// exit code.
[[nodiscard]] int run_headless(const HeadlessOptions& options,
                               const HeadlessEnvironment& environment);

}  // namespace fem::app
