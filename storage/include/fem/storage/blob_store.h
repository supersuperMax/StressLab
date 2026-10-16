#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fem::storage {

using Bytes = std::vector<std::byte>;

/// "sha256:" and 64 lower-case hex digits: how the model refers to a block.
[[nodiscard]] bool is_blob_ref(std::string_view ref) noexcept;
/// The reference of a block with these contents.
[[nodiscard]] std::string blob_ref(std::span<const std::byte> bytes);

/// Immutable blocks of bulk data addressed by the hash of their contents (docs/architecture.md
/// §3.1, "Three kinds of data"; docs/adr/0018-blob-store.md): the same block is stored once
/// however many objects, versions and branches refer to it. The reference is the hash of the
/// bytes put, whatever a store does to keep them (compression), so it does not change with the
/// version of zstd.
class BlobStore {
public:
    BlobStore() = default;
    BlobStore(const BlobStore&) = delete;
    BlobStore& operator=(const BlobStore&) = delete;
    BlobStore(BlobStore&&) = delete;
    BlobStore& operator=(BlobStore&&) = delete;
    virtual ~BlobStore() = default;

    /// Stores a block, unless one with the same contents is there, and returns its reference.
    /// Throws ProjectError(io).
    virtual std::string put(std::span<const std::byte> bytes) = 0;
    /// The contents of a block. Throws ProjectError(missing_blob) if there is none and
    /// ProjectError(corrupt_blob) if its contents do not match the reference.
    [[nodiscard]] virtual Bytes get(std::string_view ref) const = 0;
    [[nodiscard]] virtual bool contains(std::string_view ref) const = 0;
};

/// Blocks in memory, for tests and for writing a project to memory.
class MemoryBlobStore final : public BlobStore {
public:
    std::string put(std::span<const std::byte> bytes) override;
    [[nodiscard]] Bytes get(std::string_view ref) const override;
    [[nodiscard]] bool contains(std::string_view ref) const override;

    [[nodiscard]] std::size_t size() const noexcept { return blobs_.size(); }
    /// The bytes of all blocks together.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    std::map<std::string, Bytes, std::less<>> blobs_;
    std::size_t bytes_ = 0;
};

/// Blocks in a folder, one file per block: `<folder>/sha256/<2 hex digits>/<62 hex digits>`,
/// the layout of `.fem/objects` of a project. A file is one zstd frame of the block; it is
/// written whole or not at all and never changed afterwards, only removed by `remove`.
class FolderBlobStore final : public BlobStore {
public:
    explicit FolderBlobStore(std::filesystem::path folder);

    std::string put(std::span<const std::byte> bytes) override;
    [[nodiscard]] Bytes get(std::string_view ref) const override;
    [[nodiscard]] bool contains(std::string_view ref) const override;

    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return folder_; }
    /// Where the block of `ref` is kept, whether it is there or not. `ref` must be a reference.
    [[nodiscard]] std::filesystem::path file_of(std::string_view ref) const;

    /// A file of the folder: a block, or something else (a temporary file a crash left, a file
    /// with a name that is not a hash).
    struct File {
        std::filesystem::path path;
        std::string ref;  ///< The block it holds, empty if it is not a block.
        std::uintmax_t size = 0;
    };
    /// Every file of the folder, blocks by reference first. Throws ProjectError(io).
    [[nodiscard]] std::vector<File> files() const;
    /// Removes a file of `files`; false if it cannot be removed (it is not there any more).
    bool remove(const std::filesystem::path& file);

private:
    std::filesystem::path folder_;
};

}  // namespace fem::storage
