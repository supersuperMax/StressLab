#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fem::job {

/// What to run: a program, its arguments, environment and files (docs/architecture.md §6). A
/// solver adapter fills it in and gives it to the Job Manager; where the program runs is decided
/// by an `Executor`, so the same spec runs on this computer now and on a remote one later.
///
/// Text is UTF-8. The program is started directly, without a shell.
struct JobSpec {
    /// What the job does, for the "Jobs" list; the program's file name if empty.
    std::string title;
    /// An absolute path; a bare name (`solver`) is looked up in `PATH` of the job's environment;
    /// a relative path with folders is taken from `workdir`.
    std::filesystem::path program;
    /// Arguments after the program, passed as they are (no shell, no wildcards).
    std::vector<std::string> args;
    /// Variables to set (a value) or to remove (no value) on top of the inherited ones.
    std::map<std::string, std::optional<std::string>, std::less<>> environment;
    /// Start from the environment of the application; otherwise only `environment` is set.
    bool inherit_environment = true;
    /// The working folder of the program, on this computer. Inputs are read from it and outputs
    /// are expected in it; an executor that runs elsewhere copies inputs there and outputs back.
    /// Empty: the current folder of the application.
    std::filesystem::path workdir;
    /// Files the program reads; relative paths are in `workdir`. Checked before the start.
    std::vector<std::filesystem::path> inputs;
    /// Files the program writes; relative paths are in `workdir`. A job whose program succeeded
    /// but did not write all of them fails.
    std::vector<std::filesystem::path> outputs;

    friend bool operator==(const JobSpec&, const JobSpec&) = default;
};

/// `path` of a spec as it is on this computer: relative paths are in the working folder.
[[nodiscard]] std::filesystem::path resolve_in_workdir(const JobSpec& spec,
                                                       const std::filesystem::path& path);

}  // namespace fem::job
