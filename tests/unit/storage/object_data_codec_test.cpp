#include "fem/storage/object_data_codec.h"

#include "fem/core/model/array_data.h"
#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
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

// --- Named arrays of modules (task 3.1) ----------------------------------------------------------

/// Named arrays of `rows` rows: int32 nodes, int64 labels and float64 values of width 6.
std::shared_ptr<const model::ArrayData> named_arrays(std::size_t rows) {
    std::vector<std::int32_t> node(rows);
    std::vector<std::int64_t> label(rows);
    std::vector<double> values(rows * 6);
    for (std::size_t i = 0; i < rows; ++i) {
        node[i] = static_cast<std::int32_t>(i * 3);
        label[i] = static_cast<std::int64_t>(i) + (std::int64_t{1} << 40);
        for (std::size_t c = 0; c < 6; ++c) {
            values[i * 6 + c] = static_cast<double>(i) * 0.25 - static_cast<double>(c);
        }
    }
    std::vector<model::NamedArray> arrays;
    arrays.emplace_back("values", std::move(values), 6);
    arrays.emplace_back("node", std::move(node));
    arrays.emplace_back("label", std::move(label));
    return std::make_shared<const model::ArrayData>(std::move(arrays));
}

TEST(StorageObjectData, SmallNamedArraysAreOneLittleEndianTable) {
    std::vector<model::NamedArray> arrays;
    arrays.emplace_back("n", std::vector<std::int32_t>{5});
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(model::ArrayData(std::move(arrays)), blobs);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs.get(ref), bytes({'S', 'L', 'D', 'A', 'T', 'A', 0, 0,  // magic
                                     2,   0,   0,   0,                    // codec version
                                     6,   0,   0,   0,                    // named arrays
                                     1,   0,   0,   0,   0,   0,   0, 0,  // one array
                                     1,   0,   0,   0,   'n',             // its name
                                     1,   0,   0,   0,                    // int32
                                     1,   0,   0,   0,                    // width
                                     1,   0,   0,   0,   0,   0,   0, 0,  // count
                                     0,   0,   0,   0,                    // inline items
                                     5,   0,   0,   0}));
}

TEST(StorageObjectData, NamedArraysDecodeToWhatWasEncoded) {
    for (const std::size_t rows : {std::size_t{0}, std::size_t{3}, std::size_t{20000}}) {
        const auto data = named_arrays(rows);
        for (const ChunkLimits& limits : {ChunkLimits{}, small_pieces}) {
            MemoryBlobStore blobs;
            const std::string ref = put_object_data(*data, blobs, limits);
            const auto back =
                std::dynamic_pointer_cast<const model::ArrayData>(get_object_data(ref, blobs));
            ASSERT_NE(back, nullptr);
            EXPECT_EQ(describe_object_data(*back), describe_object_data(*data));
            EXPECT_EQ(put_object_data(*back, blobs, limits), ref);
            ASSERT_EQ(back->arrays().size(), 3U);
            for (std::size_t a = 0; a < 3; ++a) {
                const model::NamedArray& before = data->arrays()[a];
                const model::NamedArray& after = back->arrays()[a];
                EXPECT_EQ(after.name(), before.name());
                EXPECT_EQ(after.type(), before.type());
                EXPECT_EQ(after.width(), before.width());
                EXPECT_EQ(after.items(), before.items());
            }
        }
    }
    // 20 000 rows of six doubles are cut into pieces.
    MemoryBlobStore blobs;
    const std::string ref = put_object_data(*named_arrays(20000), blobs);
    EXPECT_GT(object_data_pieces(blobs.get(ref)).size(), 3U);

    const model::ArrayData none({});
    MemoryBlobStore empty;
    const auto back = std::dynamic_pointer_cast<const model::ArrayData>(
        get_object_data(put_object_data(none, empty), empty));
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->arrays().empty());
}

TEST(StorageObjectData, NamedArraysKeepEveryBitOfTheirItems) {
    const double nan = std::bit_cast<double>(std::uint64_t{0x7ff8'0000'dead'beef});
    std::vector<model::NamedArray> arrays;
    arrays.emplace_back("values", std::vector<double>{nan, -0.0, 5e-324});
    MemoryBlobStore blobs;
    const auto back = std::dynamic_pointer_cast<const model::ArrayData>(
        get_object_data(put_object_data(model::ArrayData(std::move(arrays)), blobs), blobs));
    ASSERT_NE(back, nullptr);
    const std::span<const double> values = back->find("values")->as<double>();
    EXPECT_EQ(std::bit_cast<std::uint64_t>(values[0]), 0x7ff8'0000'dead'beefU);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(values[1]), 0x8000'0000'0000'0000U);
    EXPECT_EQ(values[2], 5e-324);
}

TEST(StorageObjectData, NamedArraysAreDescribedWithTheirRows) {
    EXPECT_EQ(describe_object_data(*named_arrays(4)),
              (nlohmann::json{{"kind", "arrays"},
                              {"size", 4},
                              {"arrays",
                               {{"label", {{"type", "int64"}, {"width", 1}}},
                                {"node", {{"type", "int32"}, {"width", 1}}},
                                {"values", {{"type", "float64"}, {"width", 6}}}}}}));
}

TEST(StorageObjectData, RefusesDamagedNamedArrays) {
    // Two arrays "a" and "b" of one int32 each. The first array starts at 24: its name length
    // (4), name (1), type (4), width (4), count (8), storage (4), item (4); 29 bytes in all.
    std::vector<model::NamedArray> arrays;
    arrays.emplace_back("a", std::vector<std::int32_t>{1});
    arrays.emplace_back("b", std::vector<std::int32_t>{2});
    MemoryBlobStore blobs;
    const Bytes good = blobs.get(put_object_data(model::ArrayData(std::move(arrays)), blobs));
    constexpr std::size_t first = 24;
    constexpr std::size_t second = first + 29;
    ASSERT_EQ(good.size(), second + 29);

    for (std::size_t size = 0; size < good.size(); ++size) {
        ASSERT_EQ(decode_error(
                      Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size)), blobs),
                  ProjectErrc::corrupt_blob)
            << size;
    }
    Bytes unknown_type = good;
    unknown_type[first + 5] = std::byte{4};
    EXPECT_EQ(decode_error(unknown_type, blobs), ProjectErrc::corrupt_blob);
    Bytes no_width = good;
    no_width[first + 9] = std::byte{0};
    EXPECT_EQ(decode_error(no_width, blobs), ProjectErrc::corrupt_blob);
    Bytes same_names = good;
    same_names[second + 4] = std::byte{'a'};
    EXPECT_EQ(decode_error(same_names, blobs), ProjectErrc::corrupt_blob);
    Bytes out_of_order = good;
    out_of_order[first + 4] = std::byte{'c'};
    EXPECT_EQ(decode_error(out_of_order, blobs), ProjectErrc::corrupt_blob);
    Bytes bad_name = good;
    bad_name[first + 4] = std::byte{' '};
    EXPECT_EQ(decode_error(bad_name, blobs), ProjectErrc::corrupt_blob);
    Bytes other_rows = good;
    other_rows[second + 9] = std::byte{2};  // One item in rows of two.
    EXPECT_EQ(decode_error(other_rows, blobs), ProjectErrc::corrupt_blob);
}

// --- Bytes of files (task 2.5) -------------------------------------------------------------------

/// Bytes that do not repeat and do not compress, the same on every computer.
Bytes noise(std::size_t size, std::uint64_t seed) {
    Bytes result(size);
    std::uint64_t state = seed;
    for (std::byte& value : result) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        value = static_cast<std::byte>(state >> 56);
    }
    return result;
}

/// A source that gives `bytes` at most `step` bytes at a time, as a file read in odd sizes.
ByteSource source_of(const Bytes& bytes, std::size_t step) {
    auto position = std::make_shared<std::size_t>(0);
    return [&bytes, step, position](std::span<std::byte> buffer) {
        const std::size_t count = std::min({buffer.size(), step, bytes.size() - *position});
        std::memcpy(buffer.data(), bytes.data() + *position, count);
        *position += count;
        return count;
    };
}

Bytes read_back(const std::string& ref, const BlobStore& blobs) {
    Bytes out;
    const std::uint64_t count = get_byte_data(ref, blobs, [&out](std::span<const std::byte> part) {
        out.insert(out.end(), part.begin(), part.end());
    });
    EXPECT_EQ(count, out.size());
    return out;
}

TEST(StorageObjectData, BytesComeBackAsTheyWere) {
    // Empty, inside the table, just past it, several windows of the stream.
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, inline_array_bytes,
                                   inline_array_bytes + 1, std::size_t{9} * 1024 * 1024 + 7}) {
        const Bytes original = noise(size, size);
        MemoryBlobStore blobs;
        const std::string ref = put_byte_data(source_of(original, 100'003), blobs);
        EXPECT_EQ(read_back(ref, blobs), original) << size << " bytes";
        EXPECT_EQ(object_data_pieces(blobs.get(ref)).empty(), size <= inline_array_bytes);
    }
}

TEST(StorageObjectData, AStreamIsCutAsTheWholeWouldBe) {
    const Bytes original = noise(std::size_t{11} * 1024 * 1024, 7);
    MemoryBlobStore blobs;
    const std::string ref = put_byte_data(source_of(original, 4093), blobs);
    std::vector<std::string> expected;
    std::size_t start = 0;
    for (const std::size_t end : chunk_ends(original, 1)) {
        expected.push_back(
            blob_ref(std::span<const std::byte>(original).subspan(start, end - start)));
        start = end;
    }
    EXPECT_EQ(object_data_pieces(blobs.get(ref)), expected);
    // The same bytes read in other sizes give the same table.
    MemoryBlobStore again;
    EXPECT_EQ(put_byte_data(source_of(original, 1 << 20), again), ref);
}

TEST(StorageObjectData, AChangeInOnePlaceOfAFileAddsAFewPieces) {
    const Bytes original = noise(std::size_t{8} * 1024 * 1024, 11);
    Bytes changed = original;
    changed[changed.size() / 2] ^= std::byte{0xFF};
    MemoryBlobStore blobs;
    (void)put_byte_data(source_of(original, 1 << 16), blobs);
    const std::size_t before = blobs.size();
    (void)put_byte_data(source_of(changed, 1 << 16), blobs);
    // One or two pieces around the change, and the table.
    EXPECT_LE(blobs.size() - before, 3U);
}

/// Keeps only the blocks small enough to be tables; counts the rest.
class CountingStore final : public BlobStore {
public:
    std::string put(std::span<const std::byte> bytes) override {
        std::string ref = blob_ref(bytes);
        largest = std::max(largest, bytes.size());
        total += bytes.size();
        if (bytes.size() <= 1024 * 1024 && !kept_.contains(ref)) {
            kept_.emplace(ref, Bytes(bytes.begin(), bytes.end()));
        }
        return ref;
    }
    [[nodiscard]] Bytes get(std::string_view ref) const override {
        return kept_.at(std::string(ref));
    }
    [[nodiscard]] bool contains(std::string_view ref) const override {
        return kept_.contains(std::string(ref));
    }
    std::size_t largest = 0;
    std::uint64_t total = 0;

private:
    std::map<std::string, Bytes> kept_;
};

TEST(StorageObjectData, PiecesOfAFileStaySmallWhateverItsSize) {
    // A block is read whole and refused above 256 MiB (docs/adr/0018-blob-store.md), and files of
    // results are larger: no piece may grow with the file. The stream is made as it is read,
    // never in memory whole; the table grows by 40 bytes a piece, 256 MiB for a file of 400 GiB.
    constexpr std::uint64_t size = 24ULL * 1024 * 1024 + 5;
    std::uint64_t given = 0;
    std::uint64_t state = 3;
    const ByteSource generated = [&](std::span<std::byte> buffer) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - given));
        for (std::size_t i = 0; i < count; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            buffer[i] = static_cast<std::byte>(state >> 56);
        }
        given += count;
        return count;
    };
    CountingStore blobs;
    const std::string ref = put_byte_data(generated, blobs);
    EXPECT_LE(blobs.largest, ChunkLimits{}.max);
    const std::size_t pieces = object_data_pieces(blobs.get(ref)).size();
    EXPECT_EQ(blobs.total - blobs.get(ref).size(), size);
    EXPECT_GT(pieces, size / ChunkLimits{}.max);
}

TEST(StorageObjectData, BytesAndObjectDataAreNotTakenForEachOther) {
    MemoryBlobStore blobs;
    const Bytes original = noise(100'000, 5);
    const std::string file = put_byte_data(source_of(original, 1000), blobs);
    const std::string mesh = put_object_data(*small_mesh(), blobs);
    EXPECT_PROJECT_ERRC((void)get_object_data(file, blobs), ProjectErrc::corrupt_blob);
    EXPECT_PROJECT_ERRC((void)read_back(mesh, blobs), ProjectErrc::corrupt_blob);

    // A piece of another size than the table says, from a store that does not check hashes.
    const std::vector<std::string> pieces = object_data_pieces(blobs.get(file));
    ASSERT_FALSE(pieces.empty());
    class Truncating final : public BlobStore {
    public:
        Truncating(const MemoryBlobStore& blobs, std::string piece)
            : blobs_(blobs), piece_(std::move(piece)) {}
        std::string put(std::span<const std::byte> bytes) override { return blob_ref(bytes); }
        [[nodiscard]] Bytes get(std::string_view ref) const override {
            Bytes bytes = blobs_.get(ref);
            if (ref == piece_) {
                bytes.pop_back();
            }
            return bytes;
        }
        [[nodiscard]] bool contains(std::string_view ref) const override {
            return blobs_.contains(ref);
        }

    private:
        const MemoryBlobStore& blobs_;
        std::string piece_;
    };
    EXPECT_PROJECT_ERRC((void)read_back(file, Truncating(blobs, pieces.front())),
                        ProjectErrc::corrupt_blob);

    // A piece that is not in the store.
    MemoryBlobStore without_piece;
    (void)without_piece.put(blobs.get(file));
    EXPECT_PROJECT_ERRC((void)read_back(file, without_piece), ProjectErrc::missing_blob);
}

}  // namespace
}  // namespace fem::storage
