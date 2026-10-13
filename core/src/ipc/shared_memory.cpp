// Shared memory segments: named file mappings on Windows, POSIX shared memory objects elsewhere.

#include "fem/core/ipc/shared_memory.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace fem::ipc {

namespace {

std::uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

/// "sl<pid>_<n>_<random>": unique among the segments of this process, and unlikely to meet a
/// segment a crashed process with the same id left behind. At most 26 characters.
std::string new_name() {
    static std::atomic<std::uint32_t> counter{0};
    static const std::uint32_t salt = [] {
        std::random_device device;
        return static_cast<std::uint32_t>(device()) & 0xffffU;
    }();
    constexpr const char* digits = "0123456789abcdef";
    std::string suffix;
    for (int shift = 12; shift >= 0; shift -= 4) {
        suffix += digits[(salt >> shift) & 0xfU];
    }
    return "sl" + std::to_string(process_id()) + "_" + std::to_string(++counter) + "_" + suffix;
}

/// How many times `create` tries another name if one is taken.
constexpr int create_attempts = 16;

#ifdef _WIN32

std::wstring widen(std::string_view text) {
    std::wstring wide;
    for (const char c : text) {
        wide += static_cast<wchar_t>(static_cast<unsigned char>(c));  // Names are ASCII.
    }
    return wide;
}

[[noreturn]] void fail(const std::string& what) {
    throw SharedMemoryError(what + " (error " + std::to_string(GetLastError()) + ")");
}

#else

std::string posix_name(std::string_view name) {
    return "/" + std::string(name);
}

[[noreturn]] void fail(const std::string& what, int error) {
    throw SharedMemoryError(what + ": " + std::strerror(error));
}

#endif

bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 30) {
        return false;
    }
    for (const char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

}  // namespace

SharedMemory SharedMemory::create(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    SharedMemory memory;
    memory.size_ = size;
    memory.owner_ = true;
    for (int attempt = 0; attempt < create_attempts; ++attempt) {
        std::string name = new_name();
#ifdef _WIN32
        const auto wide_size = static_cast<std::uint64_t>(size);
        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(wide_size >> 32U),
            static_cast<DWORD>(wide_size & 0xffffffffU), widen(name).c_str());
        if (mapping == nullptr) {
            fail("cannot create shared memory of " + std::to_string(size) + " bytes");
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mapping);
            continue;
        }
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (view == nullptr) {
            const DWORD error = GetLastError();
            CloseHandle(mapping);
            SetLastError(error);
            fail("cannot map shared memory '" + name + "'");
        }
        memory.mapping_ = mapping;
        memory.data_ = static_cast<std::byte*>(view);
#else
        const std::string path = posix_name(name);
        const int fd = ::shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            fail("cannot create shared memory '" + name + "'", errno);
        }
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            const int error = errno;
            ::close(fd);
            ::shm_unlink(path.c_str());
            fail("cannot size shared memory '" + name + "' to " + std::to_string(size) + " bytes",
                 error);
        }
        void* view = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const int error = errno;
        ::close(fd);
        if (view == MAP_FAILED) {
            ::shm_unlink(path.c_str());
            fail("cannot map shared memory '" + name + "'", error);
        }
        memory.data_ = static_cast<std::byte*>(view);
#endif
        memory.name_ = std::move(name);
        memory.linked_ = true;
        return memory;
    }
    throw SharedMemoryError("cannot find a free name for shared memory");
}

SharedMemory SharedMemory::open(std::string_view name, std::size_t size) {
    if (!valid_name(name)) {
        throw SharedMemoryError("'" + std::string(name) + "' is not a name of shared memory");
    }
    if (size == 0) {
        size = 1;
    }
    SharedMemory memory;
    memory.name_ = std::string(name);
    memory.size_ = size;
#ifdef _WIN32
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, widen(name).c_str());
    if (mapping == nullptr) {
        fail("cannot open shared memory '" + memory.name_ + "'");
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    const DWORD error = GetLastError();
    CloseHandle(mapping);  // The view keeps the memory.
    if (view == nullptr) {
        SetLastError(error);
        fail("cannot map shared memory '" + memory.name_ + "'");
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(view, &info, sizeof(info)) == 0 || info.RegionSize < size) {
        UnmapViewOfFile(view);
        throw SharedMemoryError("shared memory '" + memory.name_ + "' is smaller than " +
                                std::to_string(size) + " bytes");
    }
    memory.data_ = static_cast<std::byte*>(view);
#else
    const std::string path = posix_name(name);
    const int fd = ::shm_open(path.c_str(), O_RDWR, 0);
    if (fd < 0) {
        fail("cannot open shared memory '" + memory.name_ + "'", errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_size < 0 ||
        static_cast<std::size_t>(info.st_size) < size) {
        ::close(fd);
        throw SharedMemoryError("shared memory '" + memory.name_ + "' is smaller than " +
                                std::to_string(size) + " bytes");
    }
    void* view = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    const int error = errno;
    ::close(fd);
    if (view == MAP_FAILED) {
        fail("cannot map shared memory '" + memory.name_ + "'", error);
    }
    memory.data_ = static_cast<std::byte*>(view);
#endif
    return memory;
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : name_(std::move(other.name_)),
      size_(std::exchange(other.size_, 0)),
      data_(std::exchange(other.data_, nullptr)),
      owner_(std::exchange(other.owner_, false)),
      linked_(std::exchange(other.linked_, false))
#ifdef _WIN32
      ,
      mapping_(std::exchange(other.mapping_, nullptr))
#endif
{
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        reset();
        name_ = std::move(other.name_);
        size_ = std::exchange(other.size_, 0);
        data_ = std::exchange(other.data_, nullptr);
        owner_ = std::exchange(other.owner_, false);
        linked_ = std::exchange(other.linked_, false);
#ifdef _WIN32
        mapping_ = std::exchange(other.mapping_, nullptr);
#endif
    }
    return *this;
}

SharedMemory::~SharedMemory() {
    reset();
}

void SharedMemory::unlink() noexcept {
    if (!owner_ || !linked_) {
        return;
    }
    linked_ = false;
#ifdef _WIN32
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
#else
    ::shm_unlink(posix_name(name_).c_str());
#endif
}

void SharedMemory::reset() noexcept {
    unlink();
    if (data_ != nullptr) {
#ifdef _WIN32
        UnmapViewOfFile(data_);
#else
        ::munmap(data_, size_);
#endif
        data_ = nullptr;
    }
    size_ = 0;
    owner_ = false;
}

}  // namespace fem::ipc
