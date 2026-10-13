#pragma once

// Child processes for the local executor. The OS-specific parts are in process_win32.cpp and
// process_posix.cpp; one of them is compiled.

#include "fem/core/job/executor.h"
#include "fem/core/job/local_executor.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::job::detail {

/// Variables in the order they were found; names are unique (as the OS compares them).
using Environment = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN32
inline constexpr char path_list_separator = ';';
#else
inline constexpr char path_list_separator = ':';
#endif

[[nodiscard]] inline std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view text) {
    return {std::u8string(text.begin(), text.end())};
}

/// What to start, resolved: an existing program, an existing absolute working folder and the
/// whole environment.
struct ProcessLaunch {
    std::filesystem::path program;
    std::vector<std::string> args;
    Environment environment;
    std::filesystem::path workdir;
    std::chrono::milliseconds terminate_grace{0};
    /// A channel to the standard input of the program; otherwise it reads an empty input.
    bool input = false;
};

// Implemented per OS.

/// The environment of this process, UTF-8.
[[nodiscard]] Environment current_environment();
/// Whether two variable names are the same variable: ignoring case on Windows.
[[nodiscard]] bool same_variable_name(std::string_view a, std::string_view b);
/// An existing file this process may start.
[[nodiscard]] bool is_runnable(const std::filesystem::path& file);
/// Names to try for a program without its extension: on Windows with ".exe" and ".com".
[[nodiscard]] std::vector<std::filesystem::path> program_candidates(
    const std::filesystem::path& program);
/// Throws JobError(launch_failed) if the program cannot be started. Without `launch.input`
/// writing to the input of the result fails.
[[nodiscard]] std::unique_ptr<InteractiveExecution> start_process(const ProcessLaunch& launch,
                                                                  OutputSink sink);

}  // namespace fem::job::detail
