#include "fem/storage/object_data_codec.h"

#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::testing::small_mesh;

/// The code of the error of reading the data whose table of contents is `table`.
ProjectErrc decode_error(const Bytes& table, MemoryBlobStore& blobs) {
    try {
        (void)get_object_data(blobs.put(table), blobs);
    } catch (const ProjectError& error) {
        return error.code();
    }
    ADD_FAILURE() << "the block was decoded";
    return ProjectErrc::io;
}

Bytes bytes(std::initializer_list<int> values) {
    Bytes result;
    for (const int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

void put_u64(Bytes& block, std::size_t at, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        block[at + i] = static_cast<std::byte>(value >> (8 * i));
    }
}

std::uint64_t get_u64(const Bytes& block, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(block[at + i]))
                 << (8 * i);
    }
    return value;
}

/// The bytes of the encoding of `data` in a fresh store.
std::string encoding(const model::ObjectData& data) {
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(data, blobs);
    std::string out;
    for (const std::string& piece : object_data_pieces(blobs.get(ref))) {
        out += piece + ";";
    }
    const Bytes table = blobs.get(ref);
    return out + std::string(reinterpret_cast<const char*>(table.data()), table.size());
}

/// Small pieces, so that tests of cutting need little data.
const ChunkLimits small_pieces{.min = 1024, .average = 4096, .max = 16384};

TEST(StorageObjectData, SmallNodeSetIsOneLittleEndianTable) {
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(model::NodeSet({2, 0x01020304}), blobs);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs.get(ref), bytes({'S', 'L', 'D', 'A', 'T', 'A', 0, 0,  // magic
                                     2,   0,   0,   0,                    // codec version
                                     2,   0,   0,   0,                    // node set
                                     2,   0,   0,   0,   0,   0,   0, 0,  // count
                                     0,   0,   0,   0,                    // inline items
                                     2,   0,   0,   0,   4,   3,   2, 1}));
    EXPECT_TRUE(object_data_pieces(blobs.get(ref)).empty());
}

TEST(StorageObjectData, EveryKindDecodesToWhatWasEncoded) {
    const GridMesh grid(30, 20, 10);
    std::vector<model::Index> many(20000);
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = static_cast<model::Index>(i * 7 % 6000);
    }
    std::vector<model::ElementSide> sides;
    for (model::Index e = 0; e < 5000; ++e) {
        sides.push_back({e, e % 6});
    }
    const std::vector<std::shared_ptr<const model::ObjectData>> data = {
        small_mesh(),
        model::testing::tiny_mesh(),
        grid.make(),
        std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 3, 4}),
        std::make_shared<const model::NodeSet>(many),
        std::make_shared<const model::ElementSet>(std::vector<model::Index>{}),
        std::make_shared<const model::ElementSet>(many),
        std::make_shared<const model::Surface>(std::vector<model::ElementSide>{{0, 1}, {2, 0}}),
        std::make_shared<const model::Surface>(sides),
    };
    for (const auto& item : data) {
        for (const ChunkLimits& limits : {ChunkLimits{}, small_pieces}) {
            MemoryBlobStore blobs;
            const std::string ref = put_object_data(*item, blobs, limits);
            const auto back = get_object_data(ref, blobs);
            EXPECT_EQ(describe_object_data(*back), describe_object_data(*item));
            EXPECT_EQ(put_object_data(*back, blobs, limits), ref);
            EXPECT_EQ(encoding(*back), encoding(*item));
        }
    }
    MemoryBlobStore blobs;
    const auto back = std::dynamic_pointer_cast<const model::Mesh>(
        get_object_data(put_object_data(*grid.make(), blobs), blobs));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->node_count(), 31U * 21U * 11U);
    EXPECT_EQ(back->element_count(), 30U * 20U * 10U);
    EXPECT_TRUE(std::equal(back->x().begin(), back->x().end(), grid.nodes.x.begin()));
    EXPECT_TRUE(std::equal(back->blocks()[0].connectivity().begin(),
                           back->blocks()[0].connectivity().end(), grid.connectivity.begin()));
}

TEST(StorageObjectData, LargeArraysAreCutIntoPieces) {
    const GridMesh grid(40, 40, 20);  // 35 301 nodes: 282 KB per coordinate.
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(*grid.make(), blobs);
    const std::vector<std::string> pieces = object_data_pieces(blobs.get(ref));
    EXPECT_GT(pieces.size(), 20U);
    for (const std::string& piece : pieces) {
        ASSERT_TRUE(blobs.contains(piece));
        EXPECT_LE(blobs.get(piece).size(), ChunkLimits{}.max);
    }
    // The table holds 40 bytes per piece and the arrays below `inline_array_bytes`.
    EXPECT_LT(blobs.get(ref).size(), 40 * pieces.size() + 200);
}

TEST(StorageObjectData, TheSameDataIsStoredOnce) {
    const GridMesh grid(40, 40, 20);
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(*grid.make(), blobs);
    const std::size_t blocks = blobs.size();
    const std::size_t bytes = blobs.bytes();
    // Another mesh with the same arrays, as in another object or another save.
    EXPECT_EQ(put_object_data(*grid.make(), blobs), ref);
    EXPECT_EQ(blobs.size(), blocks);
    EXPECT_EQ(blobs.bytes(), bytes);
}

TEST(StorageObjectData, OnePercentOfAMeshAddsLittleData) {
    GridMesh grid(60, 60, 50);  // 189 771 nodes, 180 000 hex8.
    MemoryBlobStore blobs;
    (void)put_object_data(*grid.make(), blobs);
    const std::size_t first = blobs.bytes();

    // Nodes of one region move: a few pieces of x, y and z change.
    grid.move_nodes(grid.nodes.ids.size() / 2, grid.nodes.ids.size() / 100);
    (void)put_object_data(*grid.make(), blobs);
    const std::size_t moved = blobs.bytes() - first;
    EXPECT_LT(moved, first / 20) << moved << " bytes of " << first;

    // Elements of one region go: everything after them shifts, but the cuts follow the contents.
    const std::size_t before_removal = blobs.bytes();
    grid.remove_elements(grid.element_ids.size() / 3, grid.element_ids.size() / 100);
    (void)put_object_data(*grid.make(), blobs);
    const std::size_t removed = blobs.bytes() - before_removal;
    EXPECT_LT(removed, first / 20) << removed << " bytes of " << first;
}

TEST(StorageObjectData, Descriptions) {
    EXPECT_EQ(describe_object_data(*small_mesh()),
              (nlohmann::json{{"kind", "mesh"}, {"nodes", 5}, {"elements", 3}}));
    EXPECT_EQ(describe_object_data(model::Surface({{0, 1}})),
              (nlohmann::json{{"kind", "surface"}, {"size", 1}}));
}

TEST(StorageObjectData, RefusesDamagedTables) {
    MemoryBlobStore blobs;
    const Bytes good = blobs.get(put_object_data(*small_mesh(), blobs));
    EXPECT_EQ(decode_error({}, blobs), ProjectErrc::corrupt_blob);
    EXPECT_EQ(decode_error(bytes({'S', 'L', 'D', 'A', 'T', 'A', 0, 1}), blobs),
              ProjectErrc::corrupt_blob);

    // Every truncation of a valid table is refused, never read past its end.
    for (std::size_t size = 0; size < good.size(); ++size) {
        ASSERT_EQ(decode_error(
                      Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size)), blobs),
                  ProjectErrc::corrupt_blob)
            << size;
    }
    Bytes longer = good;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decode_error(longer, blobs), ProjectErrc::corrupt_blob);

    Bytes version_1 = good;
    version_1[8] = std::byte{1};
    EXPECT_EQ(decode_error(version_1, blobs), ProjectErrc::corrupt_blob);
    Bytes other_kind = good;
    other_kind[12] = std::byte{9};
    EXPECT_EQ(decode_error(other_kind, blobs), ProjectErrc::corrupt_blob);
    Bytes other_storage = good;
    other_storage[24] = std::byte{7};  // The storage of the node ids.
    EXPECT_EQ(decode_error(other_storage, blobs), ProjectErrc::corrupt_blob);

    // A huge count does not allocate: it is checked against the size of the table.
    Bytes huge = blobs.get(put_object_data(model::NodeSet({1}), blobs));
    huge[23] = std::byte{0x7f};
    EXPECT_EQ(decode_error(huge, blobs), ProjectErrc::corrupt_blob);

    // A connectivity entry past the nodes: an invalid mesh is a damaged block.
    Bytes bad_mesh = good;
    bad_mesh[bad_mesh.size() - 4] = std::byte{99};
    EXPECT_EQ(decode_error(bad_mesh, blobs), ProjectErrc::corrupt_blob);
}

TEST(StorageObjectData, RefusesDamagedPieces) {
    // A node set in pieces: magic, version, kind (16 bytes), count (8), storage (4), the number
    // of pieces (8), then 40 bytes per piece: its digest and its item count.
    std::vector<model::Index> nodes(20000);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i] = static_cast<model::Index>(i);
    }
    MemoryBlobStore blobs;
    const Bytes good = blobs.get(put_object_data(model::NodeSet(nodes), blobs, small_pieces));
    constexpr std::size_t first_piece = 16 + 8 + 4 + 8;
    ASSERT_GT(get_u64(good, 28), 2U);
    const std::uint64_t items = get_u64(good, first_piece + 32);

    Bytes more_items = good;
    put_u64(more_items, first_piece + 32, items + 1);
    EXPECT_EQ(decode_error(more_items, blobs), ProjectErrc::corrupt_blob);
    Bytes empty_piece = good;
    put_u64(empty_piece, first_piece + 32, 0);
    EXPECT_EQ(decode_error(empty_piece, blobs), ProjectErrc::corrupt_blob);
    Bytes larger_count = good;
    put_u64(larger_count, 16, nodes.size() + 1);
    EXPECT_EQ(decode_error(larger_count, blobs), ProjectErrc::corrupt_blob);
    Bytes more_pieces = good;
    put_u64(more_pieces, 28, get_u64(good, 28) + 1);
    EXPECT_EQ(decode_error(more_pieces, blobs), ProjectErrc::corrupt_blob);

    // The first piece swapped for the second: their sizes differ from what the table says, or the
    // nodes come out in another order (still a valid set, but not this one).
    Bytes swapped = good;
    std::memcpy(swapped.data() + first_piece, good.data() + first_piece + 40, 32);
    if (get_u64(good, first_piece + 40 + 32) != items) {
        EXPECT_EQ(decode_error(swapped, blobs), ProjectErrc::corrupt_blob);
    }

    // A piece that is not in the store.
    Bytes unknown = good;
    unknown[first_piece] ^= std::byte{1};
    EXPECT_EQ(decode_error(unknown, blobs), ProjectErrc::missing_blob);
}

}  // namespace
}  // namespace fem::storage
