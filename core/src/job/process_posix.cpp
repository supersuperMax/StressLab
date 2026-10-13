// Child processes on macOS and Linux: fork and exec into a process group of their own, so that
// cancelling ends the program and every process it started.

#include "fem/core/job/error.h"

#include "process.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace fem::job::detail {

namespace {

char** process_environment() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

/// Held while this executor makes pipes and forks, so that a child forked by another job in the
/// meantime does not get our pipes before they are marked close-on-exec (macOS has no pipe2).
std::mutex& spawn_mutex() {
    static std::mutex mutex;
    return mutex;
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

[[noreturn]] void throw_errno(std::string_view what, int error) {
    throw JobError(JobErrc::launch_failed, std::string(what) + ": " + std::strerror(error));
}

struct Pipe {
    Fd read;
    Fd write;
};

Pipe make_pipe() {
    std::array<int, 2> fds{-1, -1};
#ifdef __linux__
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        throw_errno("cannot create a pipe", errno);
    }
#else
    if (::pipe(fds.data()) != 0) {
        throw_errno("cannot create a pipe", errno);
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    return Pipe{Fd(fds[0]), Fd(fds[1])};
}

/// A connected pair of sockets for the standard input of a child: `read` is its end, `write` ours.
/// A socket rather than a pipe, because writing to a socket can be told not to raise SIGPIPE when
/// the child has gone (MSG_NOSIGNAL, SO_NOSIGPIPE), without touching the signal handling of the
/// application.
Pipe make_input_channel() {
    std::array<int, 2> fds{-1, -1};
#ifdef __linux__
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) != 0) {
        throw_errno("cannot create a socket pair", errno);
    }
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        throw_errno("cannot create a socket pair", errno);
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    Pipe channel{Fd(fds[0]), Fd(fds[1])};
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(channel.write.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    // The child only reads, we only write.
    ::shutdown(channel.read.get(), SHUT_WR);
    ::shutdown(channel.write.get(), SHUT_RD);
    return channel;
}

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;  // SO_NOSIGPIPE is set on the socket instead.
#endif

/// What the child reports through the close-on-exec pipe when it cannot exec.
enum ChildStage : int { redirect = 1, change_folder = 2, exec = 3 };

/// Only async-signal-safe calls from here: the child of a multithreaded process.
[[noreturn]] void child_failed(int report, int stage) {
    const std::array<int, 2> message{stage, errno};
    [[maybe_unused]] const ssize_t written = ::write(report, message.data(), sizeof(message));
    ::_exit(127);
}

class PosixProcess final : public InteractiveExecution {
public:
    PosixProcess(pid_t pid, Fd input, Fd output, Fd error, OutputSink sink,
                 std::chrono::milliseconds terminate_grace)
        : pid_(pid),
          input_(std::move(input)),
          sink_(std::move(sink)),
          terminate_grace_(terminate_grace) {
        readers_[0] = std::thread(&PosixProcess::read, this, std::move(output),
                                  OutputStream::standard_output);
        readers_[1] =
            std::thread(&PosixProcess::read, this, std::move(error), OutputStream::standard_error);
    }
    PosixProcess(const PosixProcess&) = delete;
    PosixProcess& operator=(const PosixProcess&) = delete;
    PosixProcess(PosixProcess&&) = delete;
    PosixProcess& operator=(PosixProcess&&) = delete;

    ~PosixProcess() override {
        if (!waited_) {
            {
                const std::lock_guard lock(mutex_);
                if (!reaped_) {
                    cancel_requested_ = true;
                    killed_ = true;
                    ::kill(-pid_, SIGKILL);
                }
            }
            try {
                wait();
            } catch (...) {
                // Nothing can be reported from a destructor.
            }
        }
    }

    void cancel() noexcept override {
        {
            const std::lock_guard lock(mutex_);
            // The group is signalled only while its leader is not reaped: until then its id
            // cannot be reused by an unrelated process.
            if (reaped_ || cancel_requested_) {
                return;
            }
            cancel_requested_ = true;
            kill_deadline_ = std::chrono::steady_clock::now() + terminate_grace_;
            ::kill(-pid_, terminate_grace_.count() > 0 ? SIGTERM : SIGKILL);
        }
        changed_.notify_all();
    }

    bool write_input(std::string_view bytes) override {
        const std::lock_guard lock(input_mutex_);
        while (!bytes.empty()) {
            if (input_.get() < 0) {
                return false;
            }
            const ssize_t count = ::send(input_.get(), bytes.data(), bytes.size(), send_flags);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                input_.reset();  // The program has closed its input or ended.
                return false;
            }
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }

    void close_input() noexcept override {
        const std::lock_guard lock(input_mutex_);
        input_.reset();
    }

    ExitStatus wait() override {
        ExitStatus result;
        {
            std::unique_lock lock(mutex_);
            siginfo_t info{};
            for (;;) {
                // WNOWAIT: the leader stays a zombie, so its group can still be signalled.
                info = siginfo_t{};
                const int found =
                    ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT);
                if (found == 0 && info.si_pid == pid_) {
                    break;
                }
                if (found != 0 && errno != EINTR) {
                    break;  // Not our child any more; waitpid below tells what is known.
                }
                if (cancel_requested_ && !killed_ &&
                    std::chrono::steady_clock::now() >= kill_deadline_) {
                    ::kill(-pid_, SIGKILL);
                    killed_ = true;
                }
                changed_.wait_for(lock, std::chrono::milliseconds(10));
            }
            // Processes the program left behind would keep the pipes open and outlive the job.
            ::kill(-pid_, SIGKILL);
            int status = 0;
            pid_t reaped = -1;
            do {
                reaped = ::waitpid(pid_, &status, 0);
            } while (reaped < 0 && errno == EINTR);
            reaped_ = true;
            if (reaped == pid_) {
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.signal = WTERMSIG(status);
                }
            } else if (info.si_pid == pid_) {
                if (info.si_code == CLD_EXITED) {
                    result.exit_code = info.si_status;
                } else {
                    result.signal = info.si_status;
                }
            }
        }
        for (std::thread& reader : readers_) {
            if (reader.joinable()) {
                reader.join();
            }
        }
        waited_ = true;
        return result;
    }

private:
    void read(Fd pipe, OutputStream stream) {
        std::array<char, 16384> buffer{};
        for (;;) {
            const ssize_t count = ::read(pipe.get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return;  // Every writer has closed the pipe: the process group is gone.
            }
            try {
                sink_(stream, std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            } catch (...) {
                // The sink must not throw; the pipe is still drained so the program can go on.
            }
        }
    }

    const pid_t pid_;
    std::mutex input_mutex_;
    Fd input_;  // Our end of the program's standard input; empty without one.
    OutputSink sink_;
    std::array<std::thread, 2> readers_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::chrono::milliseconds terminate_grace_;
    bool cancel_requested_ = false;
    bool killed_ = false;
    bool reaped_ = false;
    std::chrono::steady_clock::time_point kill_deadline_;
    bool waited_ = false;
};

}  // namespace

Environment current_environment() {
    Environment environment;
    for (char** entry = process_environment(); entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text(*entry);
        const std::size_t equals = text.find('=');
        if (equals != std::string_view::npos && equals > 0) {
            environment.emplace_back(std::string(text.substr(0, equals)),
                                     std::string(text.substr(equals + 1)));
        }
    }
    return environment;
}

bool same_variable_name(std::string_view a, std::string_view b) {
    return a == b;
}

bool is_runnable(const std::filesystem::path& file) {
    std::error_code error;
    return std::filesystem::is_regular_file(file, error) && ::access(file.c_str(), X_OK) == 0;
}

std::vector<std::filesystem::path> program_candidates(const std::filesystem::path& program) {
    return {program};
}

std::unique_ptr<InteractiveExecution> start_process(const ProcessLaunch& launch, OutputSink sink) {
    // Everything the child needs is prepared before fork: it may not allocate.
    const std::string program = launch.program.native();
    const std::string workdir = launch.workdir.native();
    std::vector<std::string> arg_storage{program};
    arg_storage.insert(arg_storage.end(), launch.args.begin(), launch.args.end());
    std::vector<char*> argv;
    for (std::string& arg : arg_storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env_storage;
    for (const auto& [name, value] : launch.environment) {
        env_storage.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (std::string& variable : env_storage) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigset_t no_signals;
    sigemptyset(&no_signals);

    Pipe output;
    Pipe error;
    Pipe report;
    Fd input;
    Fd input_writer;
    pid_t pid = -1;
    {
        const std::lock_guard lock(spawn_mutex());
        output = make_pipe();
        error = make_pipe();
        report = make_pipe();
        if (launch.input) {
            Pipe channel = make_input_channel();
            input = std::move(channel.read);
            input_writer = std::move(channel.write);
        } else {
            input = Fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
            if (input.get() < 0) {
                throw_errno("cannot open /dev/null", errno);
            }
        }
        pid = ::fork();
        if (pid < 0) {
            throw_errno("cannot start '" + program + "'", errno);
        }
        if (pid == 0) {
            ::setpgid(0, 0);
            // The program starts with default signal handling, whatever the application uses.
            for (const int signal_number : {SIGPIPE, SIGTERM, SIGINT, SIGQUIT, SIGHUP, SIGCHLD}) {
                ::sigaction(signal_number, &default_action, nullptr);
            }
            ::sigprocmask(SIG_SETMASK, &no_signals, nullptr);
            if (::dup2(input.get(), STDIN_FILENO) < 0 ||
                ::dup2(output.write.get(), STDOUT_FILENO) < 0 ||
                ::dup2(error.write.get(), STDERR_FILENO) < 0) {
                child_failed(report.write.get(), redirect);
            }
            if (::chdir(workdir.c_str()) != 0) {
                child_failed(report.write.get(), change_folder);
            }
            ::execve(program.c_str(), argv.data(), envp.data());
            child_failed(report.write.get(), exec);
        }
        // Also here, so the group exists before anyone signals it.
        ::setpgid(pid, pid);
    }
    output.write.reset();
    error.write.reset();
    report.write.reset();
    input.reset();

    // The report pipe closes on a successful exec; otherwise the child says what failed.
    std::array<int, 2> message{0, 0};
    ssize_t count = 0;
    do {
        count = ::read(report.read.get(), message.data(), sizeof(message));
    } while (count < 0 && errno == EINTR);
    if (count == static_cast<ssize_t>(sizeof(message))) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        switch (message[0]) {
            case change_folder:
                throw_errno("cannot enter the working folder '" + workdir + "'", message[1]);
            case exec:
                throw_errno("cannot start '" + program + "'", message[1]);
            default:
                throw_errno("cannot redirect the output of '" + program + "'", message[1]);
        }
    }
    return std::make_unique<PosixProcess>(pid, std::move(input_writer), std::move(output.read),
                                          std::move(error.read), std::move(sink),
                                          launch.terminate_grace);
}

}  // namespace fem::job::detail
