#include "fem/storage/blob_store.h"

#include "fem/core/io/files.h"
#include "fem/storage/error.h"
#include "fem/storage/sha256.h"

#include "zstd_codec.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <tuple>
#include <utility>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

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

bool is_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

bool is_blob_ref(std::string_view ref) noexcept {
    return ref.starts_with(prefix) && ref.size() == prefix.size() + 64 &&
           is_hex(ref.substr(prefix.size()));
}

std::string blob_ref(std::span<const std::byte> bytes) {
    return std::string(prefix) + sha256_hex(bytes);
}

// --- MemoryBlobStore -----------------------------------------------------------------------------

std::string MemoryBlobStore::put(std::span<const std::byte> bytes) {
    std::string ref = blob_ref(bytes);
    if (!blobs_.contains(ref)) {
        blobs_.emplace(ref, Bytes(bytes.begin(), bytes.end()));
        bytes_ += bytes.size();
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

FolderBlobStore::FolderBlobStore(fs::path folder) : folder_(std::move(folder)) {}

fs::path FolderBlobStore::file_of(std::string_view ref) const {
    const std::string_view hex = ref.substr(prefix.size());
    return folder_ / "sha256" / std::string(hex.substr(0, 2)) / std::string(hex.substr(2));
}

std::string FolderBlobStore::put(std::span<const std::byte> bytes) {
    std::string ref = blob_ref(bytes);
    const fs::path file = file_of(ref);
    std::error_code error;
    if (fs::is_regular_file(file, error)) {
        return ref;  // Same contents: the block is there already.
    }
    const Bytes frame = detail::compress(bytes);
    fs::create_directories(file.parent_path(), error);
    const std::string_view text(reinterpret_cast<const char*>(frame.data()), frame.size());
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
    Bytes bytes;
    try {
        bytes = detail::decompress({data, text->size()});
    } catch (const ProjectError& error) {
        throw ProjectError(error.code(), std::string(ref) + ": " + error.what());
    }
    check_contents(ref, bytes);
    return bytes;
}

bool FolderBlobStore::contains(std::string_view ref) const {
    std::error_code error;
    return is_blob_ref(ref) && fs::is_regular_file(file_of(ref), error);
}

std::vector<FolderBlobStore::File> FolderBlobStore::files() const {
    std::vector<File> result;
    std::error_code error;
    if (!fs::is_directory(folder_, error)) {
        return result;
    }
    fs::recursive_directory_iterator it(folder_, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) {
            continue;
        }
        File file{it->path(), {}, it->file_size(error)};
        if (error) {
            break;
        }
        // A block only where `file_of` puts it: sha256/<2 hex>/<62 hex>.
        const fs::path relative = file.path.lexically_relative(folder_);
        const std::string ref = std::string(prefix) +
                                relative.parent_path().filename().generic_string() +
                                relative.filename().generic_string();
        if (is_blob_ref(ref) && file_of(ref) == file.path) {
            file.ref = ref;
        }
        result.push_back(std::move(file));
    }
    if (error) {
        throw ProjectError(ProjectErrc::io, "cannot list the blocks", io::display(folder_));
    }
    std::sort(result.begin(), result.end(), [](const File& a, const File& b) {
        return a.ref.empty() != b.ref.empty() ? !a.ref.empty()
                                              : std::tie(a.ref, a.path) < std::tie(b.ref, b.path);
    });
    return result;
}

bool FolderBlobStore::remove(const fs::path& file) {
    std::error_code error;
    return fs::remove(file, error) && !error;
}

}  // namespace fem::storage
