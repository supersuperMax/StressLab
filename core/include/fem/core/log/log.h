#pragma once

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Logging of the application (spdlog). Every part of the application writes to its own logger,
// named by a category ("settings", "l10n", "extension", a module id), and all loggers share the
// sinks a `Logging` object sets up: a rotating file in the user's log folder and, for headless
// runs, the standard error stream. Before that, and in tests, messages go nowhere.
namespace fem::log {

using Level = spdlog::level::level_enum;

/// "trace", "debug", "info", "warning", "error", "critical", "off" (the names spdlog writes).
[[nodiscard]] std::string_view to_string(Level level) noexcept;
/// The level named as `to_string` names it, or nothing.
[[nodiscard]] std::optional<Level> parse_level(std::string_view name) noexcept;

/// The logger of a category, created on first use and shared afterwards. Thread-safe.
[[nodiscard]] std::shared_ptr<spdlog::logger> logger(std::string_view category);

struct LogOptions {
    /// The folder of the log files, created if missing; usually `fem::user_folders().logs`.
    std::filesystem::path folder{};
    /// The current file; rotated files get a number before the extension: "stresslab.1.log".
    /// Empty means the product name in lower case plus ".log".
    std::string file_name{};
    /// A file is rotated before a message would make it larger than this.
    std::uint64_t max_file_size = 5 * 1024 * 1024;
    /// How many files are kept, the current one included; at least 1.
    std::size_t max_files = 5;
    /// Rotate when the file is opened, so each run starts a new file and the previous runs stay
    /// in the numbered ones.
    bool rotate_on_open = true;
    Level level = Level::info;
    /// Also write to the standard error stream (headless runs).
    bool console = false;
};

/// Sends the messages of all loggers to a rotating file (and the console) while it exists.
/// Several may exist at once; each gets every message of its level and above. Lines look like
///
///     2026-10-12 14:03:51.207 [warning] [settings] [12345] 'user/settings.json': ...
///
/// with the thread id in the third brackets. Every line is flushed at once, so a crash loses
/// nothing that was logged. Writes to files with Unicode paths on every OS.
class Logging {
public:
    /// Opens the log file. Throws std::runtime_error if the folder or the file cannot be created.
    explicit Logging(LogOptions options);
    Logging(const Logging&) = delete;
    Logging& operator=(const Logging&) = delete;
    Logging(Logging&&) = delete;
    Logging& operator=(Logging&&) = delete;
    /// Flushes and closes the file; loggers stop writing to it.
    ~Logging();

    /// The current log file.
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    [[nodiscard]] Level level() const noexcept;
    void set_level(Level level);
    void flush();

    /// The sinks of one `Logging`; defined in the implementation.
    struct Sinks;

private:
    std::filesystem::path file_;
    std::unique_ptr<Sinks> sinks_;
};

}  // namespace fem::log
