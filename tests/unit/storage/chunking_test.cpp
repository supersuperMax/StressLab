#include "fem/storage/chunking.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    Bytes bytes(size);
    for (std::byte& value : bytes) {
        value = static_cast<std::byte>(engine());
    }
    return bytes;
}

/// The pieces of `bytes` as text, for comparing sets of pieces.
std::vector<std::string> pieces(const Bytes& bytes, std::size_t item_size,
                                const ChunkLimits& limits = {}) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (const std::size_t end : chunk_ends(bytes, item_size, limits)) {
        result.emplace_back(reinterpret_cast<const char*>(bytes.data() + start), end - start);
        start = end;
    }
    return result;
}

TEST(StorageChunking, CutsAreWithinTheLimitsAndOnItems) {
    const ChunkLimits limits;
    for (const std::size_t item : {1U, 4U, 8U, 12U}) {
        const Bytes bytes = random_bytes(item * 300000, item);
        const std::vector<std::size_t> ends = chunk_ends(bytes, item);
        ASSERT_FALSE(ends.empty());
        EXPECT_EQ(ends.back(), bytes.size());
        std::size_t start = 0;
        for (std::size_t i = 0; i < ends.size(); ++i) {
            const std::size_t size = ends[i] - start;
            EXPECT_EQ(ends[i] % item, 0U) << item;
            EXPECT_LE(size, limits.max) << item;
            if (i + 1 < ends.size()) {
                EXPECT_GE(size, limits.min / item * item) << item;
            }
            start = ends[i];
        }
        // Pieces gather around the average.
        const double mean = static_cast<double>(bytes.size()) / static_cast<double>(ends.size());
        EXPECT_GT(mean, 0.5 * static_cast<double>(limits.average)) << item;
        EXPECT_LT(mean, 2.0 * static_cast<double>(limits.average)) << item;
    }
}

TEST(StorageChunking, SmallAndEmptyArrays) {
    EXPECT_TRUE(chunk_ends({}, 8).empty());
    const Bytes small = random_bytes(800, 1);
    EXPECT_EQ(chunk_ends(small, 8), std::vector<std::size_t>{800});
    // Data without variety (zeros) is cut at the largest size.
    const Bytes zeros(1024 * 1024);
    const std::vector<std::size_t> ends = chunk_ends(zeros, 8);
    EXPECT_EQ(ends.size(), 4U);
}

TEST(StorageChunking, CutsDependOnTheBytesOnly) {
    const Bytes bytes = random_bytes(2000000, 7);
    EXPECT_EQ(chunk_ends(bytes, 8), chunk_ends(bytes, 8));
    // Fixed forever: other cuts would make every saved mesh look changed. If this fails, the
    // gear table or the masks changed.
    const std::vector<std::size_t> ends = chunk_ends(random_bytes(400000, 42), 8);
    ASSERT_FALSE(ends.empty());
    std::uint64_t digest = 1469598103934665603ULL;  // FNV-1a of the ends.
    for (const std::size_t end : ends) {
        digest = (digest ^ end) * 1099511628211ULL;
    }
    EXPECT_EQ(ends.size(), 6U);
    EXPECT_EQ(digest, 7536442062345902763ULL) << "update only if the change is deliberate";
}

TEST(StorageChunking, AnInsertionChangesOnlyNearbyPieces) {
    const Bytes original = random_bytes(8 * 250000, 3);
    for (const std::size_t at : {std::size_t{0}, original.size() / 2, original.size() - 8}) {
        Bytes changed = original;
        const Bytes inserted = random_bytes(8 * 100, at + 1);
        changed.insert(changed.begin() + static_cast<std::ptrdiff_t>(at), inserted.begin(),
                       inserted.end());
        const std::vector<std::string> before = pieces(original, 8);
        const std::set<std::string> known(before.begin(), before.end());
        std::size_t added = 0;
        for (const std::string& piece : pieces(changed, 8)) {
            added += known.contains(piece) ? 0 : 1;
        }
        EXPECT_LE(added, 2U) << "insertion at " << at << " of " << before.size() << " pieces";
    }
}

TEST(StorageChunking, ShuffleRegroupsBytesByPosition) {
    const Bytes bytes = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const Bytes shuffled = shuffle_bytes(bytes, 4);
    EXPECT_EQ(shuffled, (Bytes{std::byte{1}, std::byte{5}, std::byte{2}, std::byte{6}, std::byte{3},
                               std::byte{7}, std::byte{4}, std::byte{8}}));
    EXPECT_EQ(unshuffle_bytes(shuffled, 4), bytes);
    for (const std::size_t item : {1U, 2U, 4U, 8U}) {
        const Bytes random = random_bytes(item * 1001, item);
        EXPECT_EQ(unshuffle_bytes(shuffle_bytes(random, item), item), random) << item;
    }
    EXPECT_TRUE(shuffle_bytes({}, 8).empty());
}

}  // namespace
}  // namespace fem::storage
