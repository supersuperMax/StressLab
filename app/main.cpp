// The `stresslab` executable. So far it only runs the embedded Python (task 1.8):
//
//     stresslab --python [python arguments]     e.g. --python -m pytest, --python script.py
//
// `--headless run-script` comes with task 1.10, the window with task 1.11.

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/product.h"
#include "fem/core/version.h"
#include "fem/python/interpreter.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void print(const std::string& text, std::FILE* stream) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fputc('\n', stream);
}

int run(const std::vector<std::string>& args) {
    fem::l10n::Localization localization;
    localization.set_language(fem::l10n::system_language());
    const auto text = [&](std::string_view key, const fem::l10n::Args& values = {}) {
        return localization.text("fem.core", key, values);
    };

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print(text("app.usage", {{"program", "stresslab"}}), stdout);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version") {
        print(std::string(fem::kProductName) + " " + std::string(fem::version()), stdout);
        return 0;
    }
    if (args[0] != "--python") {
        print(text("app.unknownOption", {{"option", args[0]}}), stderr);
        print(text("app.usage", {{"program", "stresslab"}}), stderr);
        return 2;
    }

    // The application's document: what `stresslab.cmd` changes in the scripts.
    fem::model::Document document;
    fem::command::CommandBus bus(document);
    fem::context::ContextKeys keys;
    const fem::context::CoreKeys core_keys(bus, keys);
    return fem::python::Interpreter::run_main({args.begin() + 1, args.end()}, bus, keys);
}

int run_safely(const std::vector<std::string>& args) {
    try {
        return run(args);
    } catch (const std::exception& error) {
        print(error.what(), stderr);
        return 1;
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
