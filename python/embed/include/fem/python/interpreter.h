#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/manager.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fem::python {

/// Python code run by `Interpreter` raised. The message is the formatted traceback.
class PythonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// CPython 3.12 embedded in the application, with the native part of `stresslab` built in
/// (task 1.8, docs/architecture.md §4.7).
///
/// The standard library and site-packages come from the Python 3.12 the application was built
/// with, or from the installation PYTHONHOME names. The pure Python part of `stresslab` is found
/// next to the module the build produces. Python runs in UTF-8 mode on every OS. (A Python shipped
/// with the application comes with task 3.11.)
///
/// Python can be started once per process: after it stops, it cannot start again.
class Interpreter {
public:
    /// Starts Python; `argv` becomes `sys.argv`. Throws PythonError if Python cannot start.
    explicit Interpreter(const std::vector<std::string>& argv = {});
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) = delete;
    Interpreter& operator=(Interpreter&&) = delete;
    /// Takes back what is attached and stops Python.
    ~Interpreter();

    /// Python works with the document of `bus` and with `keys` until `detach`: `stresslab.cmd`
    /// runs commands on `bus`, and `stresslab.extensions` enables and disables the modules of
    /// `extensions`, if given (without it, it raises `stresslab.ExtensionError`). They must outlive
    /// the attachment.
    void attach(command::CommandBus& bus, context::ContextKeys& keys,
                extension::ExtensionManager* extensions = nullptr);
    /// Releases what Python keeps in the attached objects; Python's handles to them close.
    void detach();

    /// Runs a script as `__main__`, like `python file`. `SystemExit` with code 0 or None is
    /// success; anything else raised throws PythonError.
    void run_file(const std::filesystem::path& file);
    /// Runs Python source in a new module namespace named `__main__`.
    void run_string(std::string_view code);

    /// Runs a script as `python file args...` would and returns the exit code: `sys.argv` is
    /// `[file, *args]`, the folder of the script comes first on `sys.path`, the script runs as the
    /// module `__main__`. `sys.exit(n)` gives n (None: 0; anything else is printed to standard
    /// error and gives 1); an exception that escapes the script is printed by `sys.excepthook`
    /// and gives 1. The standard streams of Python are flushed before it returns.
    int run_script(const std::filesystem::path& file, const std::vector<std::string>& args = {});

    /// Asks Python to run `work` soon on the thread that started it: between two instructions of
    /// the Python code running there, the way Python handles signals, or while `Task.wait` waits
    /// there. Work runs in the order asked, with the GIL; what it throws is logged (category
    /// "python"). Any thread may call this. Work that has not run when the interpreter is
    /// destroyed is dropped. Returns false once the interpreter is being destroyed.
    bool call_soon(std::function<void()> work);

    /// Runs Python as the `python` command would with these arguments (`-m pytest`, `-c code`,
    /// a script, or the interactive prompt) and returns the exit code; `bus` and `keys` are
    /// attached meanwhile. Starts and stops Python, so no `Interpreter` may exist.
    static int run_main(const std::vector<std::string>& args, command::CommandBus& bus,
                        context::ContextKeys& keys);

private:
    void* main_thread_ = nullptr;  // PyThreadState of the thread that started Python
};

}  // namespace fem::python
