// Child processes on Windows: CreateProcessW in a Job Object, so that cancelling ends the program
// and every process it started.

#include "fem/core/job/error.h"

#include "process.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fem::job::detail {

namespace {

/// Exit code given to processes ended by the executor.
constexpr UINT terminated_exit_code = 1;

/// The longest command line CreateProcessW accepts, with its terminating zero.
constexpr std::size_t max_command_line = 32767;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void reset() noexcept {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size <= 0) {
        throw JobError(JobErrc::launch_failed, "text of the job is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
    return result;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

/// "what failed: the system's text (error N)".
std::string system_error(std::string_view what, DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = length > 0 ? narrow(std::wstring_view(buffer, length)) : std::string();
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.')) {
        text.pop_back();
    }
    return std::string(what) + ": " + text + " (error " + std::to_string(code) + ")";
}

[[noreturn]] void throw_last_error(std::string_view what) {
    throw JobError(JobErrc::launch_failed, system_error(what, GetLastError()));
}

/// Appends an argument as the C runtime and CommandLineToArgvW read it back: quotes around
/// arguments with spaces, tabs or quotes (or empty ones), a backslash before every quote, and
/// backslashes doubled where a quote follows them.
void append_argument(std::wstring& line, std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        line += arg;
        return;
    }
    line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line += c;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

/// "NAME=value" strings, sorted by name ignoring case as CreateProcessW requires, each ended by
/// a zero, and a zero after the last.
std::vector<wchar_t> environment_block(const Environment& environment) {
    std::vector<std::pair<std::wstring, std::wstring>> variables;
    variables.reserve(environment.size());
    for (const auto& [name, value] : environment) {
        variables.emplace_back(widen(name), widen(value));
    }
    std::ranges::stable_sort(variables, [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.first.data(), static_cast<int>(a.first.size()),
                                    b.first.data(), static_cast<int>(b.first.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (const auto& [name, value] : variables) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

/// A pipe whose write end the child inherits.
struct Pipe {
    Handle read;
    Handle write;
};

Pipe make_pipe() {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        throw_last_error("cannot create a pipe");
    }
    Pipe pipe{Handle(read), Handle(write)};
    if (!SetHandleInformation(pipe.read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw_last_error("cannot create a pipe");
    }
    return pipe;
}

class WindowsProcess final : public Execution {
public:
    WindowsProcess(Handle process, Handle job, Handle output, Handle error, OutputSink sink)
        : process_(std::move(process)), job_(std::move(job)), sink_(std::move(sink)) {
        readers_[0] = std::thread(&WindowsProcess::read, this, std::move(output),
                                  OutputStream::standard_output);
        readers_[1] = std::thread(&WindowsProcess::read, this, std::move(error),
                                  OutputStream::standard_error);
    }
    WindowsProcess(const WindowsProcess&) = delete;
    WindowsProcess& operator=(const WindowsProcess&) = delete;
    WindowsProcess(WindowsProcess&&) = delete;
    WindowsProcess& operator=(WindowsProcess&&) = delete;

    ~WindowsProcess() override {
        if (!waited_) {
            cancel();
            WaitForSingleObject(process_.get(), INFINITE);
            join_readers();
        }
    }

    void cancel() noexcept override { TerminateJobObject(job_.get(), terminated_exit_code); }

    ExitStatus wait() override {
        WaitForSingleObject(process_.get(), INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process_.get(), &code);
        // Processes the program left behind would keep the pipes open and outlive the job.
        TerminateJobObject(job_.get(), terminated_exit_code);
        join_readers();
        waited_ = true;
        return ExitStatus{static_cast<int>(code), std::nullopt};
    }

private:
    void read(Handle pipe, OutputStream stream) {
        std::array<char, 16384> buffer{};
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                          nullptr) ||
                count == 0) {
                return;  // Every writer has closed the pipe: the process tree is gone.
            }
            try {
                sink_(stream, std::string_view(buffer.data(), count));
            } catch (...) {
                // The sink must not throw; the pipe is still drained so the program can go on.
            }
        }
    }

    void join_readers() {
        for (std::thread& reader : readers_) {
            if (reader.joinable()) {
                reader.join();
            }
        }
    }

    Handle process_;
    Handle job_;
    OutputSink sink_;
    std::array<std::thread, 2> readers_;
    bool waited_ = false;
};

}  // namespace

Environment current_environment() {
    Environment environment;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return environment;
    }
    for (const wchar_t* entry = block; *entry != L'\0';) {
        const std::wstring_view text(entry);
        // Names may start with '=' (the current folders of drives, "=C:=C:\...").
        const std::size_t equals = text.find(L'=', 1);
        if (equals != std::wstring_view::npos) {
            environment.emplace_back(narrow(text.substr(0, equals)),
                                     narrow(text.substr(equals + 1)));
        }
        entry += text.size() + 1;
    }
    FreeEnvironmentStringsW(block);
    return environment;
}

bool same_variable_name(std::string_view a, std::string_view b) {
    const std::wstring wide_a = widen(a);
    const std::wstring wide_b = widen(b);
    return CompareStringOrdinal(wide_a.data(), static_cast<int>(wide_a.size()), wide_b.data(),
                                static_cast<int>(wide_b.size()), TRUE) == CSTR_EQUAL;
}

bool is_runnable(const std::filesystem::path& file) {
    std::error_code error;
    return std::filesystem::is_regular_file(file, error);
}

std::vector<std::filesystem::path> program_candidates(const std::filesystem::path& program) {
    std::vector<std::filesystem::path> candidates{program};
    std::wstring extension = program.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    });
    if (extension != L".exe" && extension != L".com") {
        for (const wchar_t* suffix : {L".exe", L".com"}) {
            std::filesystem::path candidate = program;
            candidate += suffix;
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

std::unique_ptr<Execution> start_process(const ProcessLaunch& launch, OutputSink sink) {
    const std::wstring& program = launch.program.native();
    std::wstring command_line = L"\"" + program + L"\"";
    for (const std::string& arg : launch.args) {
        command_line += L' ';
        append_argument(command_line, widen(arg));
    }
    if (command_line.size() >= max_command_line) {
        throw JobError(JobErrc::launch_failed,
                       "the command line of '" + to_utf8(launch.program) + "' is too long");
    }
    std::vector<wchar_t> environment = environment_block(launch.environment);

    Pipe output = make_pipe();
    Pipe error = make_pipe();
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr));
    if (!input) {
        throw_last_error("cannot open NUL");
    }

    // Closing the last handle of the job kills its processes: they do not outlive the
    // application even if it crashes.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw_last_error("cannot create a job object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        throw_last_error("cannot set up a job object");
    }

    // Only these handles are inherited, so a process started at the same time by another job
    // does not get our pipes (and keep them open).
    std::array<HANDLE, 3> inherited{input.get(), output.write.get(), error.write.get()};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attributes_buffer(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        throw_last_error("cannot prepare process attributes");
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        const DWORD code = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        throw JobError(JobErrc::launch_failed,
                       system_error("cannot prepare process attributes", code));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output.write.get();
    startup.StartupInfo.hStdError = error.write.get();
    startup.lpAttributeList = attributes;

    // Suspended until it is in the job object, so nothing it starts escapes the job.
    constexpr DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    PROCESS_INFORMATION info{};
    const BOOL created =
        CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                       environment.data(), launch.workdir.c_str(), &startup.StartupInfo, &info);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        throw JobError(
            JobErrc::launch_failed,
            system_error("cannot start '" + to_utf8(launch.program) + "'", create_error));
    }
    Handle process(info.hProcess);
    const Handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const DWORD code = GetLastError();
        TerminateProcess(process.get(), terminated_exit_code);
        throw JobError(
            JobErrc::launch_failed,
            system_error("cannot put '" + to_utf8(launch.program) + "' into a job object", code));
    }
    ResumeThread(thread.get());

    // The child has its copies; ours would keep the pipes open after it ends.
    output.write.reset();
    error.write.reset();
    input.reset();
    return std::make_unique<WindowsProcess>(std::move(process), std::move(job),
                                            std::move(output.read), std::move(error.read),
                                            std::move(sink));
}

}  // namespace fem::job::detail
