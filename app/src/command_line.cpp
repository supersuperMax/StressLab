#include "command_line.h"

#include <cstddef>
#include <string_view>

namespace fem::app {

namespace {

[[noreturn]] void fail(std::string key, l10n::Args args, const std::string& what) {
    throw CommandLineError(std::move(key), std::move(args), what);
}

[[noreturn]] void unknown_option(const std::string& option) {
    fail("app.unknownOption", {{"option", option}}, "unknown option '" + option + "'");
}

HeadlessOptions parse_headless(const std::vector<std::string>& args, std::size_t at) {
    HeadlessOptions options;
    bool run_script = false;
    bool options_ended = false;
    const auto value = [&](const std::string& option) -> const std::string& {
        if (at + 1 >= args.size()) {
            fail("app.missingValue", {{"option", option}}, "option '" + option + "' needs a value");
        }
        return args[++at];
    };
    for (; at < args.size(); ++at) {
        const std::string& arg = args[at];
        const bool is_option = !options_ended && arg.size() > 1 && arg.starts_with("-");
        if (!is_option) {
            if (!run_script) {
                if (arg != "run-script") {
                    fail("app.unknownCommand", {{"command", arg}},
                         "unknown headless command '" + arg + "'");
                }
                run_script = true;
                continue;
            }
            options.script = path_from_utf8(arg);
            options.script_args.assign(args.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                                       args.end());
            return options;
        }
        if (arg == "--") {
            options_ended = true;
        } else if (arg == "--extensions") {
            options.extension_folders.push_back(path_from_utf8(value(arg)));
        } else if (arg == "--profile") {
            options.profile = path_from_utf8(value(arg));
        } else if (arg == "--journal") {
            options.journal = path_from_utf8(value(arg));
        } else if (arg == "--log-file") {
            options.log_file = path_from_utf8(value(arg));
        } else if (arg == "--log-level") {
            const std::string& name = value(arg);
            options.log_level = log::parse_level(name);
            if (!options.log_level) {
                fail("app.badLogLevel", {{"level", name}}, "unknown log level '" + name + "'");
            }
        } else {
            unknown_option(arg);
        }
    }
    if (!run_script) {
        fail("app.missingRunScript", {}, "--headless needs the command run-script");
    }
    fail("app.missingScript", {}, "run-script needs the file of a script");
}

}  // namespace

std::filesystem::path path_from_utf8(const std::string& text) {
    return {std::u8string(text.begin(), text.end())};
}

CommandLine parse_command_line(const std::vector<std::string>& args) {
    CommandLine line;
    if (args.empty()) {
        return line;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        line.mode = CommandLine::Mode::help;
    } else if (args[0] == "--version") {
        line.mode = CommandLine::Mode::version;
    } else if (args[0] == "--python") {
        line.mode = CommandLine::Mode::python;
        line.python_args.assign(args.begin() + 1, args.end());
    } else if (args[0] == "--headless") {
        line.mode = CommandLine::Mode::headless;
        line.headless = parse_headless(args, 1);
    } else {
        unknown_option(args[0]);
    }
    return line;
}

}  // namespace fem::app
