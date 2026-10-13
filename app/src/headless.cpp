#include "headless.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/job/job_keys.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/version.h"
#include "fem/python/interpreter.h"

#include "application.h"
#include "module_pump.h"

#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fem::app {

namespace {

namespace fs = std::filesystem;

/// A path for messages, UTF-8.
std::string display(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

void print_error(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

/// The options of the log: the standard output and, with --log-file, a file that grows.
log::LogOptions log_options(const HeadlessOptions& options) {
    log::LogOptions result{.level = options.log_level.value_or(log::Level::info),
                           .console = log::Console::standard_output};
    if (options.log_file) {
        const fs::path file = fs::absolute(*options.log_file);
        result.folder = file.parent_path();
        result.file_name = display(file.filename());
        result.rotate_on_open = false;
    }
    return result;
}

int run(const HeadlessOptions& options, const Environment& environment) {
    log::Logging logging(log_options(options));
    const auto log = log::logger("headless");

    Application application(environment, "headless");
    if (!options.log_level) {
        application.follow_log_level(logging);
    }
    const auto text = [&](std::string_view key, const l10n::Args& args = {}) {
        return application.localization().text("fem.core", key, args);
    };

    // What the command line names must exist: a typo is a usage error, not a failed script.
    std::error_code error;
    if (!fs::is_regular_file(options.script, error)) {
        print_error(text("app.scriptNotFound", {{"file", display(options.script)}}));
        return exit_usage;
    }
    for (const fs::path& folder : options.extension_folders) {
        if (!fs::is_directory(folder, error)) {
            print_error(text("app.folderNotFound", {{"folder", display(folder)}}));
            return exit_usage;
        }
    }
    if (options.profile && !fs::is_regular_file(*options.profile, error)) {
        print_error(text("app.fileNotFound", {{"file", display(*options.profile)}}));
        return exit_usage;
    }

    log->info("{} {}, headless: {}", kProductName, version(), display(options.script));

    application.install_modules(options.extension_folders);
    if (options.profile) {
        if (!application.enable_profile(*options.profile)) {
            return exit_failure;
        }
    } else {
        application.enable_all_modules();
    }

    command::CommandBus& bus = application.bus();
    std::optional<command::FileJournal> journal;
    Subscription journaling;
    if (options.journal) {
        journal.emplace(*options.journal);
        journaling = bus.on_journal([&](std::string_view entry) { journal->write(entry); });
    }

    python::Interpreter python;
    python.attach(bus, application.keys());
    // Lines of the script and of the log go to the standard output in the order they happen.
    python.run_string("import sys\nsys.stdout.reconfigure(line_buffering=True)\n");
    ModulePump pump(application.runtime(), bus, [&python](std::function<void()> work) {
        (void)python.call_soon(std::move(work));
    });
    const job::JobKeys job_keys(
        application.jobs(), application.keys(),
        [&python](std::function<void()> work) { (void)python.call_soon(std::move(work)); });

    application.extensions().startup();
    int code = python.run_script(options.script, options.script_args);
    pump.drain();
    log->info("the script finished with exit code {}", code);

    if (journal && !journal->good()) {
        log->error("the journal '{}' could not be written", display(journal->path()));
        if (code == exit_success) {
            code = exit_failure;
        }
    }
    python.detach();
    return code;
}

}  // namespace

int run_headless(const HeadlessOptions& options, const Environment& environment) {
    try {
        return run(options, environment);
    } catch (const std::exception& error) {
        // The log may be gone already; the standard error stream is always there.
        print_error(error.what());
        return exit_failure;
    }
}

}  // namespace fem::app
