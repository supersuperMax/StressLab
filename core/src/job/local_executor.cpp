#include "fem/core/job/local_executor.h"

#include "fem/core/job/error.h"

#include "process.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace fem::job {

namespace {

using detail::Environment;
using detail::to_utf8;

void check_text(std::string_view text, std::string_view what) {
    if (text.find('\0') != std::string_view::npos) {
        throw JobError(JobErrc::launch_failed, std::string(what) + " contains a zero byte");
    }
}

fs::path working_folder(const JobSpec& spec) {
    std::error_code error;
    fs::path folder =
        spec.workdir.empty() ? fs::current_path(error) : fs::absolute(spec.workdir, error);
    if (error || !fs::is_directory(folder, error)) {
        throw JobError(JobErrc::launch_failed,
                       "the working folder '" + to_utf8(spec.workdir) + "' does not exist");
    }
    return folder;
}

Environment environment_of(const JobSpec& spec) {
    Environment environment =
        spec.inherit_environment ? detail::current_environment() : Environment{};
    // Not a structured binding: AppleClang cannot capture one in the lambda below.
    for (const auto& variable : spec.environment) {
        const std::string& name = variable.first;
        const std::optional<std::string>& value = variable.second;
        if (name.empty() || name.find('=') != std::string::npos) {
            throw JobError(JobErrc::launch_failed, "'" + name + "' is not a variable name");
        }
        check_text(name, "a variable name");
        std::erase_if(environment, [&](const auto& inherited) {
            return detail::same_variable_name(inherited.first, name);
        });
        if (value) {
            check_text(*value, "variable '" + name + "'");
            environment.emplace_back(name, *value);
        }
    }
    return environment;
}

std::string_view variable(const Environment& environment, std::string_view name) {
    for (const auto& [key, value] : environment) {
        if (detail::same_variable_name(key, name)) {
            return value;
        }
    }
    return {};
}

std::optional<fs::path> first_runnable(const fs::path& program) {
    for (const fs::path& candidate : detail::program_candidates(program)) {
        if (detail::is_runnable(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// The program to start: a path as given (relative ones in the working folder) or a bare name
/// looked up in PATH of the job's environment, not of the application.
fs::path find_program(const fs::path& program, const fs::path& workdir,
                      const Environment& environment) {
    if (program.empty()) {
        throw JobError(JobErrc::launch_failed, "the job has no program");
    }
    if (program.has_parent_path() || program.is_absolute()) {
        const fs::path path = program.is_absolute() ? program : workdir / program;
        if (const std::optional<fs::path> found = first_runnable(path)) {
            return *found;
        }
        throw JobError(JobErrc::launch_failed,
                       "program '" + to_utf8(path) + "' does not exist or cannot be started");
    }
    std::string_view folders = variable(environment, "PATH");
    while (!folders.empty()) {
        const std::size_t end = folders.find(detail::path_list_separator);
        const std::string_view folder = folders.substr(0, end);
        folders.remove_prefix(end == std::string_view::npos ? folders.size() : end + 1);
        if (folder.empty()) {
            continue;
        }
        if (const std::optional<fs::path> found =
                first_runnable(detail::path_from_utf8(folder) / program)) {
            return *found;
        }
    }
    throw JobError(JobErrc::launch_failed,
                   "program '" + to_utf8(program) + "' is not found in PATH of the job");
}

#ifdef _WIN32
void refuse_batch_files(const fs::path& program) {
    std::string extension = to_utf8(program.extension());
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".bat" || extension == ".cmd") {
        throw JobError(JobErrc::launch_failed,
                       "batch file '" + to_utf8(program) +
                           "' cannot be started directly; run cmd.exe with it instead");
    }
}
#endif

}  // namespace

LocalExecutor::LocalExecutor(LocalExecutorOptions options) : options_(options) {}

std::unique_ptr<Execution> LocalExecutor::start(const JobSpec& spec, OutputSink sink) {
    detail::ProcessLaunch launch;
    launch.workdir = working_folder(spec);
    for (const fs::path& input : spec.inputs) {
        const fs::path file = input.is_absolute() ? input : launch.workdir / input;
        std::error_code error;
        if (!fs::exists(file, error)) {
            throw JobError(JobErrc::missing_input,
                           "input file '" + to_utf8(file) + "' does not exist");
        }
    }
    for (const std::string& arg : spec.args) {
        check_text(arg, "an argument");
    }
    launch.environment = environment_of(spec);
    launch.program = find_program(spec.program, launch.workdir, launch.environment);
#ifdef _WIN32
    refuse_batch_files(launch.program);
#endif
    launch.args = spec.args;
    launch.terminate_grace = options_.terminate_grace;
    return detail::start_process(launch, std::move(sink));
}

}  // namespace fem::job
