#include "fem/storage/blob_store.h"

#include "fem/core/io/files.h"
#include "fem/storage/error.h"
#include "fem/storage/sha256.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>

namespace fem::storage {

namespace {

constexpr std::string_view prefix = "sha256:";

[[noreturn]] void throw_missing(std::string_view ref) {
    throw ProjectError(ProjectErrc::missing_blob, "no block " + std::string(ref));
}

void check_contents(std::string_view ref, std::span<const std::byte> bytes) {
    if (blob_ref(bytes) != ref) {
        throw ProjectError(ProjectErrc::corrupt_blob,
                           "the contents of block " + std::string(ref) + " do not match its hash");
    }
}

}  // namespace

bool is_blob_ref(std::string_view ref) noexcept {
    if (!ref.starts_with(prefix) || ref.size() != prefix.size() + 64) {
        return false;
    }
    return std::all_of(ref.begin() + static_cast<std::ptrdiff_t>(prefix.size()), ref.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string blob_ref(std::span<const std::byte> bytes) {
    return std::string(prefix) + sha256_hex(bytes);
}

// --- MemoryBlobStore -----------------------------------------------------------------------------

std::string MemoryBlobStore::put(std::span<const std::byte> bytes) {
    std::string ref = blob_ref(bytes);
    if (!blobs_.contains(ref)) {
        blobs_.emplace(ref, Bytes(bytes.begin(), bytes.end()));
    }
    return ref;
}

Bytes MemoryBlobStore::get(std::string_view ref) const {
    const auto it = blobs_.find(ref);
    if (it == blobs_.end()) {
        throw_missing(ref);
    }
    return it->second;
}

bool MemoryBlobStore::contains(std::string_view ref) const {
    return blobs_.find(ref) != blobs_.end();
}

// --- FolderBlobStore -----------------------------------------------------------------------------

FolderBlobStore::FolderBlobStore(std::filesystem::path folder) : folder_(std::move(folder)) {}

std::filesystem::path FolderBlobStore::file_of(std::string_view ref) const {
    const std::string_view hex = ref.substr(prefix.size());
    return folder_ / "sha256" / std::string(hex.substr(0, 2)) / std::string(hex.substr(2));
}

std::string FolderBlobStore::put(std::span<const std::byte> bytes) {
    std::string ref = blob_ref(bytes);
    const std::filesystem::path file = file_of(ref);
    std::error_code error;
    if (std::filesystem::is_regular_file(file, error)) {
        return ref;  // Same contents: the block is there already.
    }
    std::filesystem::create_directories(file.parent_path(), error);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (error || !io::write_file(file, text)) {
        throw ProjectError(ProjectErrc::io, "cannot write the block", io::display(file));
    }
    return ref;
}

Bytes FolderBlobStore::get(std::string_view ref) const {
    if (!is_blob_ref(ref)) {
        throw_missing(ref);
    }
    const std::optional<std::string> text = io::read_file(file_of(ref));
    if (!text) {
        throw_missing(ref);
    }
    const auto* data = reinterpret_cast<const std::byte*>(text->data());
    Bytes bytes(data, data + text->size());
    check_contents(ref, bytes);
    return bytes;
}

bool FolderBlobStore::contains(std::string_view ref) const {
    std::error_code error;
    return is_blob_ref(ref) && std::filesystem::is_regular_file(file_of(ref), error);
}

}  // namespace fem::storage
