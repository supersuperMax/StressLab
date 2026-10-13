// A fake solver for the tests of the local executor (task 1.6). It runs the commands given as
// arguments one after another, e.g. `fake_process print hello progress 5 10 exit 3`:
//
//   print TEXT          TEXT and a line break to standard output
//   eprint TEXT         the same to standard error
//   partial TEXT        TEXT without a line break
//   crlf TEXT           TEXT and "\r\n"
//   progress N MS       "PROGRESS i/N" for i = 1..N, MS milliseconds apart
//   sleep MS            wait
//   exit CODE           exit with CODE at once
//   echo-args           every following argument as "[arg]" on its own line; ends the commands
//   env NAME            "NAME=value" or "NAME unset"
//   cwd                 the current folder
//   write FILE TEXT     write TEXT to FILE
//   stdin               read standard input to its end, print "stdin N bytes"
//   cat                 copy standard input to standard output as lines come, until its end
//   flood N             N long lines to standard output and to standard error, interleaved
//   spawn-heartbeat F   start a copy of itself running `heartbeat F`, sharing the output, and go on
//   heartbeat FILE      append a byte to FILE every 20 ms, forever
//
// Text is UTF-8 on every OS; on Windows the arguments are taken as UTF-16 and converted.
// Only the standard library and the OS API: the executable is copied to a folder with Cyrillic
// letters and spaces and must start there without other files.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string to_utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

void write(std::FILE* stream, const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

void sleep_ms(long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#ifdef _WIN32
std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

std::string get_env(const std::string& name, bool& found) {
    const std::wstring wide = widen(name);
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    found = size > 0;
    if (!found) {
        return {};
    }
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(size - 1);
    return narrow(value.c_str());
}

void spawn_heartbeat(const std::string& self, const std::string& file) {
    std::wstring command = L"\"" + widen(self) + L"\" heartbeat \"" + widen(file) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &process)) {
        write(stderr, "spawn failed\n");
        std::exit(90);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
#else
std::string get_env(const std::string& name, bool& found) {
    const char* value = std::getenv(name.c_str());
    found = value != nullptr;
    return found ? value : "";
}

void spawn_heartbeat(const std::string& self, const std::string& file) {
    const pid_t pid = fork();
    if (pid < 0) {
        write(stderr, "spawn failed\n");
        std::exit(90);
    }
    if (pid == 0) {
        execl(self.c_str(), self.c_str(), "heartbeat", file.c_str(), static_cast<char*>(nullptr));
        _exit(91);
    }
}
#endif

int run(const std::vector<std::string>& args) {
    const std::string& self = args[0];
    std::size_t i = 1;
    auto next = [&]() -> std::string {
        if (i >= args.size()) {
            write(stderr, "missing argument\n");
            std::exit(99);
        }
        return args[i++];
    };
    while (i < args.size()) {
        const std::string command = next();
        if (command == "print") {
            write(stdout, next() + "\n");
        } else if (command == "eprint") {
            write(stderr, next() + "\n");
        } else if (command == "partial") {
            write(stdout, next());
        } else if (command == "crlf") {
            write(stdout, next() + "\r\n");
        } else if (command == "progress") {
            const int count = std::stoi(next());
            const long ms = std::stol(next());
            for (int k = 1; k <= count; ++k) {
                write(stdout, "PROGRESS " + std::to_string(k) + "/" + std::to_string(count) + "\n");
                sleep_ms(ms);
            }
        } else if (command == "sleep") {
            sleep_ms(std::stol(next()));
        } else if (command == "exit") {
            return std::stoi(next());
        } else if (command == "echo-args") {
            while (i < args.size()) {
                write(stdout, "[" + args[i++] + "]\n");
            }
        } else if (command == "env") {
            const std::string name = next();
            bool found = false;
            const std::string value = get_env(name, found);
            write(stdout, found ? name + "=" + value + "\n" : name + " unset\n");
        } else if (command == "cwd") {
            write(stdout, to_utf8(fs::current_path()) + "\n");
        } else if (command == "write") {
            const fs::path file = from_utf8(next());
            std::ofstream(file, std::ios::binary) << next();
        } else if (command == "stdin") {
            std::size_t bytes = 0;
            char buffer[256];
            std::size_t n = 0;
            while ((n = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
                bytes += n;
            }
            write(stdout, "stdin " + std::to_string(bytes) + " bytes\n");
        } else if (command == "cat") {
            char buffer[4096];
            while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
                write(stdout, buffer);
            }
        } else if (command == "flood") {
            const int count = std::stoi(next());
            const std::string filler(100, '#');
            for (int k = 0; k < count; ++k) {
                write(stdout, "out " + std::to_string(k) + " " + filler + "\n");
                write(stderr, "err " + std::to_string(k) + " " + filler + "\n");
            }
        } else if (command == "spawn-heartbeat") {
            spawn_heartbeat(self, next());
        } else if (command == "heartbeat") {
            const fs::path file = from_utf8(next());
            for (;;) {
                std::ofstream(file, std::ios::binary | std::ios::app) << '.';
                sleep_ms(20);
            }
        } else {
            write(stderr, "unknown command " + command + "\n");
            return 98;
        }
    }
    return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<std::string> args;
    // The path of the executable itself, not argv[0] as written by the caller.
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size =
            GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (size < self.size()) {
            self.resize(size);
            break;
        }
        self.resize(self.size() * 2);
    }
    args.push_back(narrow(self.c_str()));
    for (int k = 1; k < argc; ++k) {
        args.push_back(narrow(argv[k]));
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    // The executor passes the full path of the program as argv[0].
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
