#include "fem/storage/object_data_codec.h"

#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::testing::small_mesh;

ProjectErrc decode_error(const Bytes& bytes) {
    try {
        (void)decode_object_data(bytes);
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

TEST(StorageObjectData, NodeSetBlockIsLittleEndian) {
    const model::NodeSet set({2, 0x01020304});
    EXPECT_EQ(encode_object_data(set), bytes({'S', 'L', 'D', 'A', 'T', 'A', 0, 0,  // magic
                                              1,   0,   0,   0,                    // codec version
                                              2,   0,   0,   0,                    // node set
                                              2,   0,   0,   0,   0,   0,   0, 0,  // count
                                              2,   0,   0,   0,   4,   3,   2, 1}));
}

TEST(StorageObjectData, EveryKindDecodesToWhatWasEncoded) {
    const auto mesh = small_mesh();
    const std::vector<std::shared_ptr<const model::ObjectData>> data = {
        mesh,
        model::testing::tiny_mesh(),
        std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 3, 4}),
        std::make_shared<const model::ElementSet>(std::vector<model::Index>{}),
        std::make_shared<const model::Surface>(std::vector<model::ElementSide>{{0, 1}, {2, 0}}),
    };
    for (const auto& item : data) {
        const Bytes block = encode_object_data(*item);
        const auto back = decode_object_data(block);
        EXPECT_EQ(encode_object_data(*back), block);
        EXPECT_EQ(describe_object_data(*back), describe_object_data(*item));
    }
    const auto back =
        std::dynamic_pointer_cast<const model::Mesh>(decode_object_data(encode_object_data(*mesh)));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->node_count(), 5U);
    EXPECT_EQ(back->element_count(), 3U);
    EXPECT_EQ(back->blocks()[0].type(), model::ElementType::tet4);
    EXPECT_EQ(back->z()[4], -1.0);
}

TEST(StorageObjectData, Descriptions) {
    EXPECT_EQ(describe_object_data(*small_mesh()),
              (nlohmann::json{{"kind", "mesh"}, {"nodes", 5}, {"elements", 3}}));
    EXPECT_EQ(describe_object_data(model::Surface({{0, 1}})),
              (nlohmann::json{{"kind", "surface"}, {"size", 1}}));
}

TEST(StorageObjectData, RefusesDamagedBlocks) {
    const Bytes good = encode_object_data(*small_mesh());
    EXPECT_EQ(decode_error({}), ProjectErrc::corrupt_blob);
    EXPECT_EQ(decode_error(bytes({'S', 'L', 'D', 'A', 'T', 'A', 0, 1})), ProjectErrc::corrupt_blob);

    // Every truncation of a valid block is refused, never read past its end.
    for (std::size_t size = 0; size < good.size(); ++size) {
        ASSERT_EQ(
            decode_error(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size))),
            ProjectErrc::corrupt_blob)
            << size;
    }
    Bytes longer = good;
    longer.push_back(std::byte{0});
    EXPECT_EQ(decode_error(longer), ProjectErrc::corrupt_blob);

    Bytes other_version = good;
    other_version[8] = std::byte{2};
    EXPECT_EQ(decode_error(other_version), ProjectErrc::corrupt_blob);
    Bytes other_kind = good;
    other_kind[12] = std::byte{9};
    EXPECT_EQ(decode_error(other_kind), ProjectErrc::corrupt_blob);

    // A huge count does not allocate: it is checked against the size of the block.
    Bytes huge = encode_object_data(model::NodeSet({1}));
    huge[23] = std::byte{0x7f};
    EXPECT_EQ(decode_error(huge), ProjectErrc::corrupt_blob);

    // A connectivity entry past the nodes: an invalid mesh is a damaged block.
    Bytes bad_mesh = good;
    bad_mesh[bad_mesh.size() - 4] = std::byte{99};
    EXPECT_EQ(decode_error(bad_mesh), ProjectErrc::corrupt_blob);
}

}  // namespace
}  // namespace fem::storage
