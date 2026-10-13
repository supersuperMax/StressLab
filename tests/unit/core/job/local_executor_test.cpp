// The local executor with a real child process: tests/fixtures/fake-process, copied to a folder
// with Cyrillic letters and spaces under a Cyrillic name (task 1.6). Runs on Windows, macOS and
// Linux.

#include "fem/core/job/local_executor.h"

#include "fem/core/job/error.h"
#include "fem/core/job/executor.h"
#include "fem/core/job/job_manager.h"
#include "fem/core/job/job_spec.h"
#include "fem/core/task/error.h"
#include "fem/core/task/task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <stdlib.h>  // setenv
#endif

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using fem::job::ExitStatus;
using fem::job::Job;
using fem::job::JobErrc;
using fem::job::JobError;
using fem::job::JobManager;
using fem::job::JobSpec;
using fem::job::LocalExecutor;
using fem::job::LogLine;
using fem::job::OutputStream;
using fem::job::SubmitOptions;
using fem::task::TaskControl;
using fem::task::TaskState;

constexpr auto timeout = 30s;
constexpr OutputStream out = OutputStream::standard_output;
constexpr OutputStream err = OutputStream::standard_error;

// GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

std::string utf8(const fs::path& path) {
    return utf8(std::u8string_view(path.u8string()));
}

/// The folders of this test process: `program` is the fake solver copied under a Cyrillic name
/// with a space, `workdir` is where jobs run. Copies of earlier runs are removed when they are an
/// hour old (a process killed in a test may hold its executable for a while on Windows).
struct Folders {
    fs::path root;
    fs::path program;
    fs::path workdir;
};

const Folders& folders() {
    static const Folders result = [] {
        const fs::path base = fs::temp_directory_path() / "stresslab-job-tests";
        std::error_code ignored;
        for (fs::directory_iterator it(base, ignored), end; !ignored && it != end;
             it.increment(ignored)) {
            std::error_code error;
            const auto age = fs::file_time_type::clock::now() - it->last_write_time(error);
            if (!error && age > std::chrono::hours(1)) {
                fs::remove_all(it->path(), error);
            }
        }
        std::random_device random;
        Folders made;
        made.root = base / std::to_string(random());
        const fs::path bin = made.root / fs::path(u8"решатель с пробелами");
        made.workdir = made.root / fs::path(u8"рабочая папка");
        fs::create_directories(bin);
        fs::create_directories(made.workdir);
        const fs::path source(FEM_TEST_FAKE_PROCESS);
        made.program = bin / fs::path(u8"фиктивный решатель" + source.extension().u8string());
        fs::copy_file(source, made.program, fs::copy_options::overwrite_existing);
        fs::permissions(made.program, fs::perms::owner_all, fs::perm_options::add);
        return made;
    }();
    return result;
}

JobSpec fake(std::vector<std::string> args) {
    JobSpec spec;
    spec.program = folders().program;
    spec.args = std::move(args);
    spec.workdir = folders().workdir;
    return spec;
}

/// A fresh empty file name in the working folder.
fs::path unique_file(std::u8string_view stem) {
    static std::atomic<int> counter = 0;
    return folders().workdir / fs::path(std::u8string(stem) + u8" " +
                                        fs::path(std::to_string(++counter)).u8string() + u8".txt");
}

std::vector<std::string> lines_of(const Job& job, OutputStream stream) {
    std::vector<std::string> result;
    for (const LogLine& line : job.log()) {
        if (line.stream == stream) {
            result.push_back(line.text);
        }
    }
    return result;
}

JobErrc job_error(const Job& job) {
    try {
        (void)job.task()->result();
    } catch (const JobError& error) {
        return error.code();
    }
    ADD_FAILURE() << "the job did not fail with a JobError: " << job.task()->error_message();
    return JobErrc::invalid_argument;
}

std::uintmax_t size_of(const fs::path& file) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(file, error);
    return error ? 0 : size;
}

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/// True if `file` stops growing: whoever wrote it is gone.
bool stays_the_same(const fs::path& file) {
    const std::uintmax_t before = size_of(file);
    std::this_thread::sleep_for(300ms);
    return size_of(file) == before;
}

void set_parent_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
    auto widen = [](const std::string& text) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
        result.pop_back();
        return result;
    };
    SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

class LocalExecutorTest : public ::testing::Test {
protected:
    LocalExecutorTest() { manager.add_executor(std::make_shared<LocalExecutor>(), 4); }

    std::shared_ptr<Job> run(JobSpec spec, SubmitOptions options = {}) {
        std::shared_ptr<Job> job = manager.submit(std::move(spec), std::move(options));
        EXPECT_TRUE(job->task()->wait_for(timeout)) << "the job did not finish";
        return job;
    }

    JobManager manager;
};

TEST_F(LocalExecutorTest, ReadsBothStreamsAndExitCode) {
    const std::shared_ptr<Job> job =
        run(fake({"print", "hello", "eprint", "oops", "print", "bye", "exit", "0"}));
    EXPECT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(job->exit_status(), (ExitStatus{0, std::nullopt}));
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"hello", "bye"}));
    EXPECT_EQ(lines_of(*job, err), (std::vector<std::string>{"oops"}));
}

TEST_F(LocalExecutorTest, NonZeroExitFailsJob) {
    const std::shared_ptr<Job> job = run(fake({"eprint", "no license", "exit", "3"}));
    EXPECT_EQ(job->state(), TaskState::failed);
    EXPECT_EQ(job_error(*job), JobErrc::exit_code);
    EXPECT_EQ(job->exit_status(), (ExitStatus{3, std::nullopt}));
    EXPECT_EQ(lines_of(*job, err), (std::vector<std::string>{"no license"}));
}

TEST_F(LocalExecutorTest, ProgressFromPrintedLines) {
    SubmitOptions options;
    options.on_line = [](const LogLine& line, TaskControl& control) {
        // What a solver adapter's parse_progress would do with "PROGRESS i/n".
        constexpr std::string_view prefix = "PROGRESS ";
        if (line.text.starts_with(prefix)) {
            const std::string fraction = line.text.substr(prefix.size());
            const std::size_t slash = fraction.find('/');
            control.set_progress(std::stod(fraction.substr(0, slash)) /
                                 std::stod(fraction.substr(slash + 1)));
        }
    };
    std::vector<double> seen;
    std::mutex mutex;
    std::shared_ptr<Job> job = manager.submit(fake({"progress", "5", "1"}), options);
    const fem::Subscription subscription =
        job->task()->subscribe([&](const fem::task::TaskEvent& event) {
            if (event.kind == fem::task::TaskEvent::Kind::progress && event.progress) {
                const std::lock_guard lock(mutex);
                seen.push_back(*event.progress);
            }
        });
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::succeeded);
    EXPECT_EQ(job->task()->progress(), 1.0);
    const std::lock_guard lock(mutex);
    EXPECT_FALSE(seen.empty());
    for (std::size_t i = 1; i < seen.size(); ++i) {
        EXPECT_LT(seen[i - 1], seen[i]);
    }
}

TEST_F(LocalExecutorTest, CyrillicAndSpacesInProgramWorkdirArgumentsAndFiles) {
    const fs::path input = unique_file(u8"входной файл");
    std::ofstream(input) << "model\n";
    const fs::path output = unique_file(u8"результат расчёта");
    JobSpec spec = fake({"cwd", "write", utf8(output.filename()), "напряжения 42 МПа", "echo-args",
                         "деталь №1", utf8(input.filename())});
    spec.inputs = {input.filename()};
    spec.outputs = {output.filename()};
    const std::shared_ptr<Job> job = run(spec);
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();

    const std::vector<std::string> lines = lines_of(*job, out);
    ASSERT_EQ(lines.size(), 3U);
    const fs::path cwd(std::u8string(lines[0].begin(), lines[0].end()));
    EXPECT_TRUE(fs::equivalent(cwd, folders().workdir)) << lines[0];
    EXPECT_EQ(lines[1], "[деталь №1]");
    EXPECT_EQ(lines[2], "[" + utf8(input.filename()) + "]");
    std::ifstream written(output, std::ios::binary);
    std::string content;
    std::getline(written, content);
    EXPECT_EQ(content, "напряжения 42 МПа");
}

TEST_F(LocalExecutorTest, PassesArgumentsExactly) {
    // Quotes and backslashes are where Windows command lines go wrong; no shell is involved, so
    // wildcards and variables stay as they are.
    const std::vector<std::string> args{
        "",
        "a b",
        "a\"b",
        "a\\",
        "a\\\\\"b",
        "\\\\server\\share\\",
        "tab\there",
        "{}*?[x]",
        "$HOME",
        "%PATH%",
        "'single'",
        "end with space ",
        "\"",
        "\\\"",
        "C:\\Program Files\\x",
        "многие слова и \"кавычки\"",
    };
    std::vector<std::string> command{"echo-args"};
    command.insert(command.end(), args.begin(), args.end());
    const std::shared_ptr<Job> job = run(fake(command));
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    std::vector<std::string> expected;
    for (const std::string& arg : args) {
        expected.push_back("[" + arg + "]");
    }
    EXPECT_EQ(lines_of(*job, out), expected);
}

TEST_F(LocalExecutorTest, SetsAndRemovesEnvironment) {
    set_parent_env("STRESSLAB_TEST_PARENT", "от родителя");
    set_parent_env("STRESSLAB_TEST_REMOVED", "x");
    JobSpec spec = fake({"env", "STRESSLAB_TEST_SET", "env", "STRESSLAB_TEST_REMOVED", "env",
                         "STRESSLAB_TEST_PARENT"});
    spec.environment = {{"STRESSLAB_TEST_SET", "значение с пробелом"},
                        {"STRESSLAB_TEST_REMOVED", std::nullopt}};
    const std::shared_ptr<Job> inherited = run(spec);
    ASSERT_EQ(inherited->state(), TaskState::succeeded) << inherited->task()->error_message();
    EXPECT_EQ(lines_of(*inherited, out),
              (std::vector<std::string>{"STRESSLAB_TEST_SET=значение с пробелом",
                                        "STRESSLAB_TEST_REMOVED unset",
                                        "STRESSLAB_TEST_PARENT=от родителя"}));

    spec.inherit_environment = false;
    const std::shared_ptr<Job> clean = run(spec);
    ASSERT_EQ(clean->state(), TaskState::succeeded) << clean->task()->error_message();
    EXPECT_EQ(
        lines_of(*clean, out),
        (std::vector<std::string>{"STRESSLAB_TEST_SET=значение с пробелом",
                                  "STRESSLAB_TEST_REMOVED unset", "STRESSLAB_TEST_PARENT unset"}));
}

#ifdef _WIN32
TEST_F(LocalExecutorTest, EnvironmentNamesIgnoreCaseOnWindows) {
    set_parent_env("STRESSLAB_TEST_CASE", "old");
    JobSpec spec = fake({"env", "STRESSLAB_TEST_CASE"});
    spec.environment = {{"stresslab_test_case", "new"}};
    const std::shared_ptr<Job> job = run(spec);
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"STRESSLAB_TEST_CASE=new"}));
}

TEST_F(LocalExecutorTest, RefusesBatchFiles) {
    const fs::path script = folders().workdir / "script.cmd";
    std::ofstream(script) << "@echo off\r\n";
    JobSpec spec;
    spec.program = script;
    spec.workdir = folders().workdir;
    const std::shared_ptr<Job> job = run(spec);
    EXPECT_EQ(job->state(), TaskState::failed);
    EXPECT_EQ(job_error(*job), JobErrc::launch_failed);
}
#endif

TEST_F(LocalExecutorTest, FindsProgramInPathOfTheJob) {
    JobSpec spec = fake({"print", "found"});
    spec.environment = {{"PATH", utf8(folders().program.parent_path())}};
    // A bare name; on Windows without ".exe", which is added as from PATHEXT.
    spec.program = folders().program.stem();
    const std::shared_ptr<Job> job = run(spec);
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"found"}));
}

TEST_F(LocalExecutorTest, StandardInputIsEmpty) {
    const std::shared_ptr<Job> job = run(fake({"stdin"}));
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"stdin 0 bytes"}));
}

TEST_F(LocalExecutorTest, LineBreaksOfAnyKind) {
    const std::shared_ptr<Job> job =
        run(fake({"crlf", "windows", "print", "unix", "partial", "no break"}));
    ASSERT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"windows", "unix", "no break"}));
}

TEST_F(LocalExecutorTest, LotsOfOutputOnBothStreamsDoesNotBlock) {
    std::atomic<int> out_lines = 0;
    std::atomic<int> err_lines = 0;
    std::shared_ptr<Job> job = manager.submit(fake({"flood", "5000"}));
    const fem::Subscription subscription = job->subscribe_log(
        [&](const LogLine& line) { ++(line.stream == out ? out_lines : err_lines); });
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::succeeded);
    // Lines handed out before the listener subscribed are only in the log.
    const std::vector<LogLine> log = job->log();
    EXPECT_EQ(log.size(), 10000U);
    EXPECT_LE(out_lines.load(), 5000);
    EXPECT_LE(err_lines.load(), 5000);
    EXPECT_EQ(lines_of(*job, out).back().substr(0, 9), "out 4999 ");
    EXPECT_EQ(lines_of(*job, err).back().substr(0, 9), "err 4999 ");
}

TEST_F(LocalExecutorTest, ProblemsBeforeStartFailJob) {
    JobSpec no_program = fake({});
    no_program.program = "stresslab-no-such-program";
    EXPECT_EQ(job_error(*run(no_program)), JobErrc::launch_failed);

    JobSpec missing_file = fake({});
    missing_file.program = folders().workdir / fs::path(u8"нет такого.exe");
    EXPECT_EQ(job_error(*run(missing_file)), JobErrc::launch_failed);

    JobSpec no_workdir = fake({});
    no_workdir.workdir = folders().root / fs::path(u8"нет папки");
    EXPECT_EQ(job_error(*run(no_workdir)), JobErrc::launch_failed);

    JobSpec no_input = fake({});
    no_input.inputs = {fs::path(u8"нет входа.dat")};
    const std::shared_ptr<Job> job = run(no_input);
    EXPECT_EQ(job_error(*job), JobErrc::missing_input);
    EXPECT_EQ(job->exit_status(), std::nullopt);
}

TEST_F(LocalExecutorTest, CancelInTheMiddleOfWork) {
    std::atomic<int> progress_lines = 0;
    SubmitOptions options;
    options.on_line = [&](const LogLine& line, TaskControl&) {
        if (line.text.starts_with("PROGRESS")) {
            ++progress_lines;
        }
    };
    const std::shared_ptr<Job> job = manager.submit(fake({"progress", "1000", "20"}), options);
    ASSERT_TRUE(wait_until([&] { return progress_lines.load() >= 2; }));
    const auto cancelled_at = std::chrono::steady_clock::now();
    EXPECT_TRUE(job->cancel());
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_LT(std::chrono::steady_clock::now() - cancelled_at, 5s) << "must not wait for the end";
    EXPECT_EQ(job->state(), TaskState::cancelled);
    EXPECT_THROW((void)job->task()->result(), fem::task::TaskCancelled);
    ASSERT_TRUE(job->exit_status().has_value()) << "the program had started";
    EXPECT_FALSE(job->exit_status()->succeeded());
    EXPECT_LT(progress_lines.load(), 1000);
}

TEST_F(LocalExecutorTest, CancelKillsProcessTree) {
    const fs::path heartbeat = unique_file(u8"пульс");
    const std::shared_ptr<Job> job =
        manager.submit(fake({"spawn-heartbeat", utf8(heartbeat), "progress", "1000", "20"}));
    ASSERT_TRUE(wait_until([&] { return size_of(heartbeat) > 2; })) << "the child did not start";
    job->cancel();
    ASSERT_TRUE(job->task()->wait_for(timeout));
    EXPECT_EQ(job->state(), TaskState::cancelled);
    EXPECT_TRUE(stays_the_same(heartbeat)) << "a child of the job is still running";
}

TEST_F(LocalExecutorTest, ChildLeftBehindDoesNotKeepJobRunning) {
    // The child shares the output pipes: if it were left alive, the job would never end.
    const fs::path heartbeat = unique_file(u8"сирота");
    const std::shared_ptr<Job> job = run(
        fake({"spawn-heartbeat", utf8(heartbeat), "sleep", "200", "print", "done", "exit", "0"}));
    EXPECT_EQ(job->state(), TaskState::succeeded) << job->task()->error_message();
    EXPECT_EQ(lines_of(*job, out), (std::vector<std::string>{"done"}));
    EXPECT_TRUE(stays_the_same(heartbeat)) << "a child of the job is still running";
}

TEST_F(LocalExecutorTest, RunsSeveralJobsAtOnce) {
    std::vector<std::shared_ptr<Job>> jobs;
    for (int i = 0; i < 4; ++i) {
        jobs.push_back(manager.submit(fake({"sleep", "100", "print", std::to_string(i)})));
    }
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(jobs[i]->task()->wait_for(timeout));
        EXPECT_EQ(jobs[i]->state(), TaskState::succeeded);
        EXPECT_EQ(lines_of(*jobs[i], out), (std::vector<std::string>{std::to_string(i)}));
    }
}

TEST(LocalExecutor, CancelBeforeWait) {
    LocalExecutor executor({.terminate_grace = 100ms});
    EXPECT_EQ(executor.id(), "local");
    std::mutex mutex;
    std::string output;
    std::unique_ptr<fem::job::Execution> execution = executor.start(
        fake({"print", "started", "sleep", "60000"}), [&](OutputStream, std::string_view bytes) {
            const std::lock_guard lock(mutex);
            output += bytes;
        });
    ASSERT_TRUE(wait_until([&] {
        const std::lock_guard lock(mutex);
        return !output.empty();
    }));
    execution->cancel();
    execution->cancel();
    const ExitStatus status = execution->wait();
    EXPECT_FALSE(status.succeeded());
}

TEST(LocalExecutor, DestroyingRunningExecutionStopsIt) {
    const fs::path heartbeat = unique_file(u8"брошенный");
    {
        LocalExecutor executor;
        const std::unique_ptr<fem::job::Execution> execution =
            executor.start(fake({"spawn-heartbeat", utf8(heartbeat), "sleep", "60000"}),
                           [](OutputStream, std::string_view) {});
        ASSERT_TRUE(wait_until([&] { return size_of(heartbeat) > 2; }));
    }
    EXPECT_TRUE(stays_the_same(heartbeat));
}

// --- Interactive programs (task 1.9) -------------------------------------------------------------

/// Output of an interactive program collected for the test.
struct Collected {
    std::mutex mutex;
    std::string text;

    fem::job::OutputSink sink() {
        return [this](OutputStream stream, std::string_view bytes) {
            if (stream == out) {
                const std::lock_guard lock(mutex);
                text += bytes;
            }
        };
    }
    std::string get() {
        const std::lock_guard lock(mutex);
        return text;
    }
};

TEST(LocalExecutorInteractive, TalksToProgramThroughItsInput) {
    LocalExecutor executor;
    Collected output;
    const std::unique_ptr<fem::job::InteractiveExecution> execution =
        executor.start_interactive(fake({"cat"}), output.sink());
    ASSERT_TRUE(execution->write_input("первая строка\n"));
    ASSERT_TRUE(wait_until([&] { return output.get() == "первая строка\n"; }));
    // An answer comes before the next question is asked: a conversation, not one batch.
    ASSERT_TRUE(execution->write_input("second\n"));
    ASSERT_TRUE(wait_until([&] { return output.get() == "первая строка\nsecond\n"; }));
    execution->close_input();
    execution->close_input();
    EXPECT_EQ(execution->wait(), (ExitStatus{0, std::nullopt}));
    EXPECT_FALSE(execution->write_input("after\n"));
}

TEST(LocalExecutorInteractive, WritesLargeInputWhileProgramReads) {
    LocalExecutor executor;
    Collected output;
    const std::unique_ptr<fem::job::InteractiveExecution> execution =
        executor.start_interactive(fake({"stdin"}), output.sink());
    const std::string line(999, 'x');
    std::string input;
    for (int i = 0; i < 4000; ++i) {
        input += line + "\n";  // 4 MB: far more than a pipe holds.
    }
    ASSERT_TRUE(execution->write_input(input));
    execution->close_input();
    EXPECT_TRUE(execution->wait().succeeded());
    EXPECT_EQ(output.get(), "stdin " + std::to_string(input.size()) + " bytes\n");
}

TEST(LocalExecutorInteractive, WritingToEndedProgramFailsQuietly) {
    LocalExecutor executor;
    Collected output;
    const std::unique_ptr<fem::job::InteractiveExecution> execution =
        executor.start_interactive(fake({"exit", "4"}), output.sink());
    EXPECT_EQ(execution->wait().exit_code, 4);
    // No SIGPIPE on POSIX: the test process would die here otherwise.
    bool written = true;
    for (int i = 0; i < 100 && written; ++i) {
        written = execution->write_input(std::string(65536, 'y'));
    }
    EXPECT_FALSE(written);
}

TEST(LocalExecutorInteractive, CancelEndsProgramWaitingForInput) {
    LocalExecutor executor({.terminate_grace = 100ms});
    Collected output;
    const std::unique_ptr<fem::job::InteractiveExecution> execution =
        executor.start_interactive(fake({"cat"}), output.sink());
    execution->cancel();
    EXPECT_FALSE(execution->wait().succeeded());
}

TEST(LocalExecutorInteractive, PlainStartHasNoInput) {
    LocalExecutor executor;
    Collected output;
    const std::unique_ptr<fem::job::Execution> execution =
        executor.start(fake({"stdin"}), output.sink());
    EXPECT_TRUE(execution->wait().succeeded());
    EXPECT_EQ(output.get(), "stdin 0 bytes\n");
}

TEST(LocalExecutor, StartThrowsForMissingInput) {
    LocalExecutor executor;
    JobSpec spec = fake({});
    spec.inputs = {"absent.dat"};
    try {
        (void)executor.start(spec, [](OutputStream, std::string_view) {});
        FAIL() << "started without its input";
    } catch (const JobError& error) {
        EXPECT_EQ(error.code(), JobErrc::missing_input);
    }
}

}  // namespace
