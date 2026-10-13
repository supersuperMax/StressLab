#pragma once

#include <QLockFile>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace fem::app {

/// The log file of one running window. Several windows (processes) may run at once; each writes
/// and rotates a file of its own, so that no window renames the file another one writes (on
/// Windows that fails, and the file grows past its limit). The first window gets "stresslab.log",
/// the next ones "stresslab-2.log", "stresslab-3.log"... (ADR 0012). A slot is held by a lock
/// file next to it while the window runs; the lock of a process that died is taken over.
struct LogSlot {
    /// The name of the current log file in the folder, e.g. "stresslab-2.log".
    std::string file_name;
    /// Held for as long as the window writes the file.
    std::unique_ptr<QLockFile> lock;
};

/// Takes the first free slot of `folder` (created if missing); nothing if all `max_slots` are
/// taken or the folder cannot be used.
[[nodiscard]] std::optional<LogSlot> take_log_slot(const std::filesystem::path& folder,
                                                   int max_slots = 8);

}  // namespace fem::app
