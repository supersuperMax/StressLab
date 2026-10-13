#include "executable_path.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fem::app {

std::filesystem::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            throw std::runtime_error("cannot find the executable of the process");
        }
        if (size < buffer.size()) {
            buffer.resize(size);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("cannot find the executable of the process");
    }
    std::error_code error;
    const std::filesystem::path path(buffer.data());
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::absolute(path) : canonical;
#else
    std::error_code error;
    const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        throw std::runtime_error("cannot find the executable of the process: " + error.message());
    }
    return path;
#endif
}

}  // namespace fem::app
