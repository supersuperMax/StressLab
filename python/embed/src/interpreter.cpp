// Python.h comes first, through nanobind.
#include "fem/python/interpreter.h"

#include "fem/python/session.h"

#include <nanobind/nanobind.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// Defined by NB_MODULE(_stresslab, ...) in python/bindings/module.cpp.
extern "C" PyObject* PyInit__stresslab();

namespace fem::python {

namespace nb = nanobind;

namespace {

#ifdef _WIN32
constexpr char path_separator = ';';
#else
constexpr char path_separator = ':';
#endif

/// Where the pure Python part of `stresslab` is, and the Python the application was built with:
/// its installation, its site-packages (of a virtual environment, if it is one) and executable.
constexpr const char* site_dir = FEM_PYTHON_SITE_DIR;
constexpr const char* built_home = FEM_PYTHON_HOME;
constexpr const char* built_site_packages = FEM_PYTHON_SITE_PACKAGES;
constexpr const char* built_executable = FEM_PYTHON_EXECUTABLE;

/// A variable of the environment as UTF-8; empty if it is not set.
std::string environment(const char* name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(wide_name.c_str(), value.data(), size));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(),
                        bytes, nullptr, nullptr);
    return utf8;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at startup
    return value != nullptr ? value : "";
#endif
}

void check(PyStatus status, PyConfig* config = nullptr) {
    if (PyStatus_Exception(status) == 0) {
        return;
    }
    if (config != nullptr) {
        PyConfig_Clear(config);
    }
    std::string message = "cannot start Python";
    if (status.func != nullptr) {
        message += std::string(" (") + status.func + ")";
    }
    if (status.err_msg != nullptr) {
        message += std::string(": ") + status.err_msg;
    }
    throw PythonError(message);
}

/// Starts Python. `argv[0]` is the program; with `parse_argv` the rest is a `python` command line.
void start(const std::vector<std::string>& argv, bool parse_argv) {
    if (Py_IsInitialized() != 0) {
        throw PythonError("Python is running already");
    }
    static bool registered = false;
    if (!registered) {
        if (PyImport_AppendInittab("_stresslab", &PyInit__stresslab) != 0) {
            throw PythonError("cannot register the module _stresslab");
        }
        registered = true;
    }

    // UTF-8 mode: command lines, paths and text files are UTF-8 on every OS (CLAUDE.md).
    PyPreConfig preconfig;
    PyPreConfig_InitPythonConfig(&preconfig);
    preconfig.utf8_mode = 1;
    check(Py_PreInitialize(&preconfig));

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = parse_argv ? 1 : 0;
    config.install_signal_handlers = parse_argv ? 1 : 0;

    std::vector<char*> args;
    args.reserve(argv.size());
    for (const std::string& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    check(PyConfig_SetBytesArgv(&config, static_cast<Py_ssize_t>(args.size()), args.data()),
          &config);

    // The installation to take the standard library from. Without PYTHONHOME it is the Python
    // the application was built with; getpath cannot find it from our executable.
    const bool own_home = environment("PYTHONHOME").empty();
    if (own_home) {
        check(PyConfig_SetBytesString(&config, &config.home, built_home), &config);
        check(PyConfig_SetBytesString(&config, &config.executable, built_executable), &config);
    }

    // The `stresslab` package goes before what PYTHONPATH adds.
    std::string pythonpath = site_dir;
    if (const std::string user = environment("PYTHONPATH"); !user.empty()) {
        pythonpath += path_separator + user;
    }
    check(PyConfig_SetBytesString(&config, &config.pythonpath_env, pythonpath.c_str()), &config);

    check(Py_InitializeFromConfig(&config), &config);
    PyConfig_Clear(&config);

    // The site-packages of the build Python, after the standard library and with their .pth
    // files: with the home set, getpath does not see a virtual environment.
    if (own_home) {
        try {
            const nb::object add = nb::module_::import_("site").attr("addsitedir");
            std::string_view rest = built_site_packages;
            while (!rest.empty()) {
                const std::size_t end = std::min(rest.find(path_separator), rest.size());
                add(nb::str(rest.data(), end));
                rest.remove_prefix(std::min(end + 1, rest.size()));
            }
        } catch (nb::python_error& error) {
            throw PythonError(error.what());
        }
    }
}

std::vector<std::string> with_program(std::vector<std::string> args) {
    args.insert(args.begin(), "stresslab");
    return args;
}

/// Runs `call` with the GIL; a Python exception becomes PythonError, SystemExit(0) is success.
template <class Call>
void run_python(Call&& call) {
    const nb::gil_scoped_acquire gil;
    try {
        call();
    } catch (nb::python_error& error) {
        if (error.matches(PyExc_SystemExit)) {
            const nb::object code = error.value().attr("code");
            if (code.is_none() || (PyLong_Check(code.ptr()) && nb::cast<long>(code) == 0)) {
                return;
            }
        }
        throw PythonError(error.what());
    }
}

}  // namespace

Interpreter::Interpreter(const std::vector<std::string>& argv) {
    start(with_program(argv), false);
    // Other threads (listeners of tasks) take the GIL when they need it.
    main_thread_ = PyEval_SaveThread();
}

Interpreter::~Interpreter() {
    PyEval_RestoreThread(static_cast<PyThreadState*>(main_thread_));
    Session::instance().detach();
    // The module's atexit hook releases what Python keeps in the core before Python stops.
    Py_FinalizeEx();
}

void Interpreter::attach(command::CommandBus& bus, context::ContextKeys& keys) {
    const nb::gil_scoped_acquire gil;
    Session::instance().attach(bus, keys);
}

void Interpreter::detach() {
    const nb::gil_scoped_acquire gil;
    Session::instance().detach();
}

void Interpreter::run_file(const std::filesystem::path& file) {
    run_python([&] {
        const std::u8string text = file.u8string();
        const nb::str path(reinterpret_cast<const char*>(text.data()), text.size());
        nb::module_::import_("runpy").attr("run_path")(path, nb::arg("run_name") = "__main__");
    });
}

void Interpreter::run_string(std::string_view code) {
    run_python([&] {
        const nb::object module = nb::module_::import_("types").attr("ModuleType")("__main__");
        const nb::object globals = module.attr("__dict__");
        globals["__builtins__"] = nb::module_::import_("builtins");
        nb::module_::import_("builtins").attr("exec")(nb::str(code.data(), code.size()), globals);
    });
}

int Interpreter::run_main(const std::vector<std::string>& args, command::CommandBus& bus,
                          context::ContextKeys& keys) {
    start(with_program(args), true);
    Session::instance().attach(bus, keys);
    // Runs the program and stops Python; the module's atexit hook releases what Python keeps.
    const int code = Py_RunMain();
    Session::instance().detach();
    return code;
}

}  // namespace fem::python
