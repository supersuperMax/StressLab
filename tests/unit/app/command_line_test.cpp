// The command line of `stresslab` (tasks 1.8 and 1.10, ADR 0011).

#include "command_line.h"

#include "executable_path.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using fem::app::CommandLine;
using fem::app::CommandLineError;
using fem::app::parse_command_line;
using fem::log::Level;
using Mode = fem::app::CommandLine::Mode;

namespace {

std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// The key of the error the command line gives.
std::string error_of(const std::vector<std::string>& args) {
    try {
        (void)parse_command_line(args);
    } catch (const CommandLineError& error) {
        return error.key();
    }
    return "no error";
}

TEST(CommandLine, HelpVersionAndPython) {
    EXPECT_EQ(parse_command_line({}).mode, Mode::help);
    EXPECT_EQ(parse_command_line({"--help"}).mode, Mode::help);
    EXPECT_EQ(parse_command_line({"-h"}).mode, Mode::help);
    EXPECT_EQ(parse_command_line({"--version"}).mode, Mode::version);
    const CommandLine python = parse_command_line({"--python", "-m", "pytest", "-q"});
    EXPECT_EQ(python.mode, Mode::python);
    EXPECT_EQ(python.python_args, (std::vector<std::string>{"-m", "pytest", "-q"}));
}

TEST(CommandLine, HeadlessRunScriptWithArguments) {
    const std::string script = utf8(u8"папка с пробелами/расчёт.py");
    const CommandLine line =
        parse_command_line({"--headless", "run-script", script, "a", "--journal", "-x"});
    EXPECT_EQ(line.mode, Mode::headless);
    EXPECT_EQ(line.headless.script, fs::path(u8"папка с пробелами/расчёт.py"));
    // After the script everything is the script's, options of the application included.
    EXPECT_EQ(line.headless.script_args, (std::vector<std::string>{"a", "--journal", "-x"}));
    EXPECT_FALSE(line.headless.journal);
    EXPECT_TRUE(line.headless.extension_folders.empty());
}

TEST(CommandLine, HeadlessOptionsBeforeAndAfterRunScript) {
    const CommandLine line = parse_command_line(
        {"--headless", "--extensions", "one", "--log-level", "warning", "run-script",
         "--extensions", utf8(u8"два"), "--journal", utf8(u8"журнал.slpy"), "--profile", "p.json",
         "--log-file", "run.log", "script.py"});
    const fem::app::HeadlessOptions& options = line.headless;
    EXPECT_EQ(options.extension_folders,
              (std::vector<fs::path>{fs::path("one"), fs::path(u8"два")}));
    EXPECT_EQ(options.log_level, Level::warn);
    EXPECT_EQ(options.journal, fs::path(u8"журнал.slpy"));
    EXPECT_EQ(options.profile, fs::path("p.json"));
    EXPECT_EQ(options.log_file, fs::path("run.log"));
    EXPECT_EQ(options.script, fs::path("script.py"));
    EXPECT_TRUE(options.script_args.empty());
}

TEST(CommandLine, DoubleDashEndsTheOptions) {
    const CommandLine line = parse_command_line({"--headless", "run-script", "--", "-odd.py", "x"});
    EXPECT_EQ(line.headless.script, fs::path("-odd.py"));
    EXPECT_EQ(line.headless.script_args, (std::vector<std::string>{"x"}));
}

TEST(CommandLine, EveryLogLevel) {
    for (const auto& [name, level] :
         std::vector<std::pair<std::string, Level>>{{"trace", Level::trace},
                                                    {"debug", Level::debug},
                                                    {"info", Level::info},
                                                    {"warning", Level::warn},
                                                    {"error", Level::err},
                                                    {"critical", Level::critical},
                                                    {"off", Level::off}}) {
        EXPECT_EQ(parse_command_line({"--headless", "--log-level", name, "run-script", "s.py"})
                      .headless.log_level,
                  level)
            << name;
    }
}

TEST(CommandLine, Errors) {
    EXPECT_EQ(error_of({"--window"}), "app.unknownOption");
    EXPECT_EQ(error_of({"script.py"}), "app.unknownOption");
    EXPECT_EQ(error_of({"--headless"}), "app.missingRunScript");
    EXPECT_EQ(error_of({"--headless", "--journal", "j.slpy"}), "app.missingRunScript");
    EXPECT_EQ(error_of({"--headless", "run"}), "app.unknownCommand");
    EXPECT_EQ(error_of({"--headless", "run-script"}), "app.missingScript");
    EXPECT_EQ(error_of({"--headless", "run-script", "--"}), "app.missingScript");
    EXPECT_EQ(error_of({"--headless", "run-script", "--journal"}), "app.missingValue");
    EXPECT_EQ(error_of({"--headless", "--verbose", "run-script", "s.py"}), "app.unknownOption");
    EXPECT_EQ(error_of({"--headless", "--log-level", "loud", "run-script", "s.py"}),
              "app.badLogLevel");
}

TEST(CommandLine, ErrorsCarryWhatTheMessageNeeds) {
    try {
        (void)parse_command_line({"--headless", "run-script", "--profile"});
        FAIL() << "no error";
    } catch (const CommandLineError& error) {
        EXPECT_EQ(error.key(), "app.missingValue");
        EXPECT_EQ(std::get<std::string>(error.args().at("option")), "--profile");
        EXPECT_NE(std::string(error.what()).find("--profile"), std::string::npos);
    }
}

TEST(ExecutablePath, IsThisTestProgram) {
    const fs::path path = fem::app::executable_path();
    EXPECT_TRUE(path.is_absolute()) << path;
    EXPECT_TRUE(fs::is_regular_file(path)) << path;
    EXPECT_EQ(path.stem(), fs::path("stresslab_app_tests"));
}

}  // namespace
