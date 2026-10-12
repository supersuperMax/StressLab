#include "shared_library.h"

#include "fem/core/extension/error.h"

#include "files.h"

#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fem::extension::detail {

namespace {

[[noreturn]] void fail(const std::filesystem::path& file, const std::string& reason) {
    throw ExtensionError(ExtensionErrc::activation_failed,
                         "cannot load '" + display(file) + "': " + reason);
}

#if defined(_WIN32)

/// The message of the last Windows error, as UTF-8.
std::string last_error() {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr,
                                             0, nullptr, nullptr);
        text.resize(static_cast<std::size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), size,
                            nullptr, nullptr);
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.')) {
        text.pop_back();
    }
    return "Windows error " + std::to_string(code) + (text.empty() ? "" : ": " + text);
}

#endif

}  // namespace

std::shared_ptr<SharedLibrary> SharedLibrary::open(const std::filesystem::path& file) {
    const std::filesystem::path absolute = std::filesystem::absolute(file).lexically_normal();
#if defined(_WIN32)
    // No message boxes for missing dependencies: the error is reported to the caller.
    DWORD previous_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    HMODULE handle =
        LoadLibraryExW(absolute.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const std::string reason = handle == nullptr ? last_error() : std::string();
    SetThreadErrorMode(previous_mode, nullptr);
    if (handle == nullptr) {
        fail(absolute, reason);
    }
#else
    void* handle = dlopen(absolute.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = dlerror();
        fail(absolute, reason != nullptr ? reason : "unknown error");
    }
#endif
    return std::shared_ptr<SharedLibrary>(new SharedLibrary(handle));
}

std::string_view SharedLibrary::suffix() noexcept {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

SharedLibrary::Symbol SharedLibrary::symbol(const char* name) const {
#if defined(_WIN32)
    return reinterpret_cast<Symbol>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return reinterpret_cast<Symbol>(dlsym(handle_, name));
#endif
}

}  // namespace fem::extension::detail
