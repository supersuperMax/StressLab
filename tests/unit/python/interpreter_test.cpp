// Scripts run as `python file args` would run them, and work handed to the thread of Python
// (task 1.10, ADR 0011). The scripts are in a folder with Cyrillic letters and spaces.

#include "fem/python/interpreter.h"

#include "python_test_support.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using fem::python::testing::interpreter;

namespace {

std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// A path as Python source text: a raw string with forward slashes.
std::string python_literal(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return "r'" + std::string(text.begin(), text.end()) + "'";
}

class InterpreterTest : public ::testing::Test {
protected:
    InterpreterTest()
        : folder(fs::temp_directory_path() / fs::path(u8"stresslab скрипты тест") / "run") {
        fs::remove_all(folder.parent_path());
        fs::create_directories(folder);
    }
    ~InterpreterTest() override {
        std::error_code ignored;
        fs::remove_all(folder.parent_path(), ignored);
    }
    InterpreterTest(const InterpreterTest&) = delete;
    InterpreterTest& operator=(const InterpreterTest&) = delete;
    InterpreterTest(InterpreterTest&&) = delete;
    InterpreterTest& operator=(InterpreterTest&&) = delete;

    fs::path write(const fs::path& name, std::string_view text) const {
        const fs::path path = folder / name;
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    [[nodiscard]] std::string read(const fs::path& name) const {
        std::ifstream in(folder / name, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    /// Runs the script with Python's standard error caught; returns the exit code.
    int run(const fs::path& script, const std::vector<std::string>& args = {}) {
        interpreter().run_string("import io, sys\nsys.stderr = io.StringIO()\n");
        const int code = interpreter().run_script(script, args);
        interpreter().run_string(
            "import pathlib, sys\npathlib.Path(" + python_literal(folder / "stderr.txt") +
            ").write_text(sys.stderr.getvalue(), encoding='utf-8', newline='')\n"
            "sys.stderr = sys.__stderr__\n");
        error_output = read("stderr.txt");
        return code;
    }

    fs::path folder;
    std::string error_output;
};

TEST_F(InterpreterTest, AScriptThatEndsGivesZero) {
    EXPECT_EQ(run(write("ok.py", "x = 1\n")), 0);
    EXPECT_EQ(error_output, "");
}

TEST_F(InterpreterTest, SysExitGivesItsCode) {
    EXPECT_EQ(run(write("three.py", "import sys\nsys.exit(3)\n")), 3);
    EXPECT_EQ(run(write("none.py", "raise SystemExit(None)\n")), 0);
    EXPECT_EQ(run(write("text.py", "import sys\nsys.exit('gave up')\n")), 1);
    EXPECT_EQ(error_output, "gave up\n");
}

TEST_F(InterpreterTest, AnExceptionIsPrintedWithItsTracebackAndGivesOne) {
    const fs::path script =
        write(u8"ошибка.py", "def f():\n    raise ValueError('broken')\n\nf()\n");
    EXPECT_EQ(run(script), 1);
    EXPECT_NE(error_output.find("Traceback"), std::string::npos) << error_output;
    EXPECT_NE(error_output.find(utf8(u8"ошибка.py\", line 2, in f")), std::string::npos)
        << error_output;
    EXPECT_NE(error_output.find("ValueError: broken"), std::string::npos) << error_output;
    // The frames are those of the script, not of the machinery that ran it.
    EXPECT_EQ(error_output.find("runpy"), std::string::npos) << error_output;
}

TEST_F(InterpreterTest, TheScriptSeesItsArgumentsItsFolderAndItsName) {
    write("helper.py", "VALUE = 'from the folder of the script'\n");
    const fs::path script = write(u8"скрипт.py", R"(import json, sys
import helper
with open(sys.argv[1], "w", encoding="utf-8") as out:
    json.dump({"argv": sys.argv, "path0": sys.path[0], "name": __name__, "file": __file__,
               "helper": helper.VALUE}, out, ensure_ascii=False)
)");
    const std::string out = utf8((folder / "out.json").u8string());
    EXPECT_EQ(run(script, {out, utf8(u8"аргумент с пробелом")}), 0) << error_output;
    interpreter().run_string(R"(import json, os, pathlib
data = json.loads(pathlib.Path()" +
                             python_literal(folder / "out.json") +
                             R"().read_text(encoding="utf-8"))
script = )" + python_literal(script) +
                             R"(
assert os.path.normcase(data["argv"][0]) == os.path.normcase(os.path.normpath(script)), data
assert data["argv"][2] == "аргумент с пробелом", data
assert os.path.normcase(data["path0"]) == os.path.normcase(os.path.dirname(os.path.normpath(script))), data
assert data["name"] == "__main__", data
assert data["file"] == data["argv"][0], data
assert data["helper"] == "from the folder of the script", data
)");
}

TEST_F(InterpreterTest, WorkRunsOnThePythonThreadBetweenInstructions) {
    std::atomic<bool> ran_on_main{false};
    const std::thread::id main = std::this_thread::get_id();
    std::thread other([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_TRUE(interpreter().call_soon([&] {
            ran_on_main = std::this_thread::get_id() == main;
            interpreter().run_string("import builtins\nbuiltins.stresslab_called = True\n");
        }));
    });
    // Python is busy with a loop of its own; the work comes in between.
    interpreter().run_string(R"(import builtins, time
deadline = time.monotonic() + 30
while not getattr(builtins, "stresslab_called", False):
    assert time.monotonic() < deadline, "the work did not run"
    time.sleep(0.005)
del builtins.stresslab_called
)");
    other.join();
    EXPECT_TRUE(ran_on_main);
}

TEST_F(InterpreterTest, WorkRunsWhileATaskIsWaitedFor) {
    // The task finishes only by the work: a wait that does not run it would never end.
    interpreter().run_string(R"(import builtins, stresslab
builtins.stresslab_task, builtins.stresslab_control = stresslab.make_task("waiting")
)");
    std::thread other([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_TRUE(interpreter().call_soon([&] {
            interpreter().run_string(
                "import builtins\n"
                "builtins.stresslab_control.start()\n"
                "builtins.stresslab_control.succeed(42)\n");
        }));
    });
    interpreter().run_string(R"(import builtins
assert builtins.stresslab_task.result(timeout=30) == 42
del builtins.stresslab_task, builtins.stresslab_control
)");
    other.join();
}

TEST_F(InterpreterTest, WorkThatThrowsDoesNotStopTheWorkAfterIt) {
    EXPECT_TRUE(interpreter().call_soon([] { throw std::runtime_error("broken work"); }));
    EXPECT_TRUE(interpreter().call_soon(
        [] { interpreter().run_string("import builtins\nbuiltins.stresslab_after = True\n"); }));
    interpreter().run_string(R"(import builtins, time
deadline = time.monotonic() + 30
while not getattr(builtins, "stresslab_after", False):
    assert time.monotonic() < deadline, "the work did not run"
    time.sleep(0.005)
del builtins.stresslab_after
)");
}

}  // namespace
