// The `stresslab` executable:
//
// - `stresslab [folder.slproj]`: the window (task 1.11, ADR 0012), with a project open (task 2.1);
// - `stresslab --python [python arguments]`: the embedded Python (task 1.8);
// - `stresslab --headless [options] run-script <file.py> [args]`: a script without the window
//   (task 1.10, ADR 0011).

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/product.h"
#include "fem/core/user_folders.h"
#include "fem/core/version.h"
#include "fem/python/interpreter.h"

#include "application.h"
#include "command_line.h"
#include "executable_path.h"
#include "headless.h"
#ifdef FEM_WITH_WINDOW
#include "window.h"
#endif

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void print(const std::string& text, std::FILE* stream) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fputc('\n', stream);
}

fem::app::Environment application_environment() {
    fem::app::Environment environment;
    environment.executable = fem::app::executable_path();
    environment.builtin_extensions = fem::app::path_from_utf8(FEM_BUILTIN_EXTENSIONS_DIR);
    try {
        environment.user_settings = fem::user_folders().config / "settings.json";
    } catch (const std::exception&) {
        // No home folder (a service account): no settings of the user.
    }
    return environment;
}

#ifdef FEM_WITH_WINDOW
/// The executable is a console program, for --headless and --python. Started for the window from
/// Explorer, it gets a console of its own, which goes away here; started from a terminal, it
/// keeps the terminal for its messages.
void detach_from_console() {
#ifdef _WIN32
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1) {
        FreeConsole();
    }
#endif
}
#endif

int run(const std::vector<std::string>& args) {
    fem::l10n::Localization localization;
    localization.set_language(fem::l10n::system_language());
    const auto text = [&](std::string_view key, const fem::l10n::Args& values = {}) {
        return localization.text("fem.core", key, values);
    };

    fem::app::CommandLine line;
    try {
        line = fem::app::parse_command_line(args);
    } catch (const fem::app::CommandLineError& error) {
        print(text(error.key(), error.args()), stderr);
        print(text("app.usage", {{"program", "stresslab"}}), stderr);
        return fem::app::exit_usage;
    }

    switch (line.mode) {
        case fem::app::CommandLine::Mode::window:
#ifdef FEM_WITH_WINDOW
            detach_from_console();
            return fem::app::run_window(application_environment(), line.project);
#else
            // A build without the GUI: tell what it can do.
            print(text("app.usage", {{"program", "stresslab"}}), stdout);
            return fem::app::exit_usage;
#endif
        case fem::app::CommandLine::Mode::help:
            print(text("app.usage", {{"program", "stresslab"}}), stdout);
            return fem::app::exit_success;
        case fem::app::CommandLine::Mode::version:
            print(std::string(fem::kProductName) + " " + std::string(fem::version()), stdout);
            return fem::app::exit_success;
        case fem::app::CommandLine::Mode::headless:
            return fem::app::run_headless(line.headless, application_environment());
        case fem::app::CommandLine::Mode::python:
            break;
    }

    // The application's document: what `stresslab.cmd` changes in the scripts.
    fem::model::Document document;
    fem::command::CommandBus bus(document);
    fem::context::ContextKeys keys;
    const fem::context::CoreKeys core_keys(bus, keys);
    return fem::python::Interpreter::run_main(line.python_args, bus, keys);
}

int run_safely(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& error) {
        print(error.what(), stderr);
        return fem::app::exit_failure;
    }
}

}  // namespace

#ifdef _WIN32
// Arguments arrive as UTF-16 on Windows; the rest of the program works with UTF-8.
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), size, nullptr, nullptr);
        }
        args.push_back(std::move(arg));
    }
    return run_safely(args);
}
#else
int main(int argc, char** argv) {
    return run_safely(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
