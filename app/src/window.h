#pragma once

#include "application.h"

#include <filesystem>
#include <optional>

namespace fem::app {

/// Opens the window (task 1.11, ADR 0012) and runs until it is closed; returns the exit code.
/// The `Application` with every compatible module enabled, the theme of the system, the shell
/// strings in the language of the system (or of the setting "general.language"), the workspaces
/// of the user, a log file of its own (`take_log_slot`), and the loop of out-of-process modules in
/// the event loop of Qt. `project`, if given, is the project folder to open (task 2.1).
[[nodiscard]] int run_window(Environment environment,
                             const std::optional<std::filesystem::path>& project = {});

}  // namespace fem::app
