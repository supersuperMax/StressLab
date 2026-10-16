#include "zstd_codec.h"

#include "fem/storage/error.h"

#include <zstd.h>

#include <memory>
#include <string>

namespace fem::storage::detail {

namespace {

/// zstd's default: fast to write, and blocks are written once and read many times.
constexpr int level = 3;

[[noreturn]] void throw_corrupt(const std::string& message) {
    throw ProjectError(ProjectErrc::corrupt_blob, "zstd frame: " + message);
}

struct ContextDeleter {
    void operator()(ZSTD_CCtx* context) const noexcept { ZSTD_freeCCtx(context); }
    void operator()(ZSTD_DCtx* context) const noexcept { ZSTD_freeDCtx(context); }
};

}  // namespace

Bytes compress(std::span<const std::byte> bytes) {
    const std::unique_ptr<ZSTD_CCtx, ContextDeleter> context(ZSTD_createCCtx());
    if (!context) {
        throw ProjectError(ProjectErrc::io, "cannot compress a block: no memory");
    }
    Bytes frame(ZSTD_compressBound(bytes.size()));
    const std::size_t size = ZSTD_compressCCtx(context.get(), frame.data(), frame.size(),
                                               bytes.data(), bytes.size(), level);
    if (ZSTD_isError(size) != 0U) {
        throw ProjectError(ProjectErrc::io,
                           std::string("cannot compress a block: ") + ZSTD_getErrorName(size));
    }
    frame.resize(size);
    return frame;
}

Bytes decompress(std::span<const std::byte> frame) {
    if (ZSTD_findFrameCompressedSize(frame.data(), frame.size()) != frame.size()) {
        throw_corrupt("not exactly one whole frame");
    }
    const unsigned long long size = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) {
        throw_corrupt("the frame does not state its size");
    }
    if (size > max_block_size) {
        throw_corrupt("the frame claims " + std::to_string(size) + " bytes");
    }
    Bytes bytes(static_cast<std::size_t>(size));
    const std::unique_ptr<ZSTD_DCtx, ContextDeleter> context(ZSTD_createDCtx());
    if (!context) {
        throw ProjectError(ProjectErrc::io, "cannot unpack a block: no memory");
    }
    const std::size_t written =
        ZSTD_decompressDCtx(context.get(), bytes.data(), bytes.size(), frame.data(), frame.size());
    if (ZSTD_isError(written) != 0U) {
        throw_corrupt(ZSTD_getErrorName(written));
    }
    if (written != bytes.size()) {
        throw_corrupt("the frame unpacks to another size than it states");
    }
    return bytes;
}

}  // namespace fem::storage::detail
