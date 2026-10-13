#pragma once

#include "application.h"
#include "command_line.h"

namespace fem::app {

/// Exit codes of `stresslab --headless` besides those of the script (`sys.exit(n)`).
inline constexpr int exit_success = 0;
/// The script raised, or the application failed (a journal that could not be written).
inline constexpr int exit_failure = 1;
/// A wrong command line: an unknown option, a script or a folder that does not exist.
inline constexpr int exit_usage = 2;

/// Runs a script without the window (task 1.10, ADR 0011): the `Application` (document, Command
/// Bus, context keys, the settings of the user, the modules: built-in, from `--extensions`,
/// enabled all or by `--profile`), the embedded Python attached to the document. The log goes to
/// the standard output (and `--log-file`); the script's own output goes there too, line by line.
/// Returns the exit code.
[[nodiscard]] int run_headless(const HeadlessOptions& options, const Environment& environment);

}  // namespace fem::app
