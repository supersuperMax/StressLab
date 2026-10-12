#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

namespace fem::extension::detail {

/// A shared library loaded into the process, behind one interface for Windows, macOS and Linux.
///
/// It is never unloaded: unloading native code from a running process is unreliable (pointers
/// into it may survive anywhere, docs/architecture.md §4.6, item 2), so a library stays loaded
/// until the process exits.
class SharedLibrary {
public:
    /// Loads the library; its dependencies are looked up next to it first. Throws
    /// ExtensionError(activation_failed) with the reason given by the OS.
    [[nodiscard]] static std::shared_ptr<SharedLibrary> open(const std::filesystem::path& file);
    /// File name suffix of shared libraries on this OS: ".dll", ".dylib" or ".so".
    [[nodiscard]] static std::string_view suffix() noexcept;

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    ~SharedLibrary() = default;

    /// The exported function `name` as `Function`, or nullptr if there is none.
    template <class Function>
    [[nodiscard]] Function function(const char* name) const {
        return reinterpret_cast<Function>(symbol(name));
    }

private:
    using Symbol = void (*)();

    explicit SharedLibrary(void* handle) noexcept : handle_(handle) {}
    [[nodiscard]] Symbol symbol(const char* name) const;

    void* handle_;
};

}  // namespace fem::extension::detail
