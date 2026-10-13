#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fem::ipc {

/// A shared memory operation failed: the segment cannot be created, opened or mapped.
class SharedMemoryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A named block of memory that other processes on this computer can map: how large arrays go
/// to and from out-of-process modules without passing through a pipe (docs/architecture.md §4.4).
///
/// Windows: a named file mapping backed by the paging file. macOS and Linux: a POSIX shared
/// memory object (`shm_open`). The name is the same text on every OS and in Python's
/// `multiprocessing.shared_memory` (which adds the leading '/' on POSIX itself); names are short,
/// because macOS allows only 31 characters.
///
/// The memory lives while a process has it mapped. The process that created a segment *unlinks*
/// it (`unlink`, or the destructor of the creator): then it cannot be opened by name any more,
/// but processes that have it mapped keep their memory. Move-only.
class SharedMemory {
public:
    /// A new zero-filled segment of `size` bytes (at least one) with a unique name. Throws
    /// SharedMemoryError.
    [[nodiscard]] static SharedMemory create(std::size_t size);
    /// Maps the first `size` bytes of a segment created by another process. Throws
    /// SharedMemoryError if there is no such segment or it is smaller.
    [[nodiscard]] static SharedMemory open(std::string_view name, std::size_t size);

    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    /// Unmaps the memory; the creator also unlinks the name.
    ~SharedMemory();

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    /// True for the segment this object created.
    [[nodiscard]] bool owner() const noexcept { return owner_; }

    /// Removes the name, so the segment cannot be opened any more; the memory stays mapped here
    /// and in other processes. Does nothing for a segment this object did not create. On
    /// Windows the name goes away with the last handle, so only the creator's handle is closed.
    void unlink() noexcept;

private:
    SharedMemory() = default;
    void reset() noexcept;

    std::string name_;
    std::size_t size_ = 0;
    std::byte* data_ = nullptr;
    bool owner_ = false;
    bool linked_ = false;
#ifdef _WIN32
    void* mapping_ = nullptr;  // HANDLE of the file mapping.
#endif
};

}  // namespace fem::ipc
