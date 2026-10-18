// Bulk data of objects of modules (docs/adr/0023-structural-model-and-results-ir.md): named arrays
// with the same number of rows, kept without copies and stored bit for bit.

#include "fem/core/model/array_data.h"

#include "fem/core/model/document.h"

#include "model_test_support.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using namespace fem::model;
using fem::model::testing::BusEditor;
using fem::model::testing::module_a;
using fem::model::testing::module_object;
using fem::model::testing::small_mesh;

namespace {

/// Values of a vector field on three nodes: the nodes and three components each.
std::shared_ptr<const ArrayData> field_data() {
    std::vector<NamedArray> arrays;
    arrays.emplace_back("values", std::vector<double>{1, 2, 3, 4, 5, 6, 7, 8, 9}, 3);
    arrays.emplace_back("node", std::vector<std::int32_t>{0, 2, 4});
    return std::make_shared<const ArrayData>(std::move(arrays));
}

TEST(ArrayData, ScalarTypesHaveStableNames) {
    for (const ScalarType type : {ScalarType::int32, ScalarType::int64, ScalarType::float64}) {
        EXPECT_EQ(parse_scalar_type(to_string(type)), type);
    }
    EXPECT_EQ(to_string(ScalarType::float64), "float64");
    EXPECT_FALSE(parse_scalar_type("float32").has_value());
}

TEST(ArrayData, ArrayHasRowsOfItsWidth) {
    const NamedArray array("S", std::vector<double>(12, 0.5), 6);
    EXPECT_EQ(array.name(), "S");
    EXPECT_EQ(array.type(), ScalarType::float64);
    EXPECT_EQ(array.width(), 6U);
    EXPECT_EQ(array.size(), 12U);
    EXPECT_EQ(array.rows(), 2U);
    EXPECT_EQ(array.as<double>().size(), 12U);
    EXPECT_MODEL_ERROR((void)array.as<std::int32_t>(), ModelErrc::invalid_data);

    EXPECT_EQ(NamedArray("ids", std::vector<std::int64_t>{7}).type(), ScalarType::int64);
    EXPECT_EQ(NamedArray("empty", std::vector<std::int32_t>{}, 4).rows(), 0U);
}

TEST(ArrayData, ArrayTakesItemsWithoutCopying) {
    std::vector<double> values(1000, 1.0);
    const double* items = values.data();
    const NamedArray array("values", std::move(values), 10);
    EXPECT_EQ(array.as<double>().data(), items);

    std::vector<NamedArray> arrays;
    arrays.push_back(NamedArray("values", std::vector<double>(30, 2.0), 3));
    const double* stored = arrays.front().as<double>().data();
    const ArrayData data(std::move(arrays));
    EXPECT_EQ(data.find("values")->as<double>().data(), stored);
}

TEST(ArrayData, ArrayRefusesBadNamesAndWidths) {
    for (const char* name : {"", "a b", "S.11", "напряжение", "x-y"}) {
        SCOPED_TRACE(name);
        EXPECT_MODEL_ERROR((void)NamedArray(name, std::vector<double>{}), ModelErrc::invalid_data);
    }
    EXPECT_MODEL_ERROR((void)NamedArray(std::string(65, 'a'), std::vector<double>{}),
                       ModelErrc::invalid_data);
    EXPECT_NO_THROW((void)NamedArray(std::string(64, 'a'), std::vector<double>{}));
    EXPECT_NO_THROW((void)NamedArray("Node_2", std::vector<double>{}));

    EXPECT_MODEL_ERROR((void)NamedArray("values", std::vector<double>{1, 2}, 0),
                       ModelErrc::invalid_data);
    EXPECT_MODEL_ERROR((void)NamedArray("values", std::vector<double>{1, 2, 3, 4}, 3),
                       ModelErrc::invalid_data);
}

TEST(ArrayData, ArraysAreOrderedByNameAndFound) {
    const auto data = field_data();
    EXPECT_EQ(data->rows(), 3U);
    ASSERT_EQ(data->arrays().size(), 2U);
    EXPECT_EQ(data->arrays()[0].name(), "node");
    EXPECT_EQ(data->arrays()[1].name(), "values");
    ASSERT_NE(data->find("values"), nullptr);
    EXPECT_EQ(data->find("values")->width(), 3U);
    EXPECT_EQ(data->find("node")->as<std::int32_t>()[2], 4);
    EXPECT_EQ(data->find("point"), nullptr);
    EXPECT_EQ(data->find(""), nullptr);

    const ArrayData empty({});
    EXPECT_EQ(empty.rows(), 0U);
    EXPECT_TRUE(empty.arrays().empty());
}

TEST(ArrayData, ArraysNeedUniqueNamesAndTheSameRows) {
    std::vector<NamedArray> same_name;
    same_name.emplace_back("values", std::vector<double>{1});
    same_name.emplace_back("values", std::vector<std::int32_t>{1});
    EXPECT_MODEL_ERROR((void)ArrayData(std::move(same_name)), ModelErrc::invalid_data);

    std::vector<NamedArray> other_rows;
    other_rows.emplace_back("values", std::vector<double>{1, 2, 3, 4, 5, 6}, 3);  // 2 rows
    other_rows.emplace_back("node", std::vector<std::int32_t>{0, 1, 2});          // 3 rows
    EXPECT_MODEL_ERROR((void)ArrayData(std::move(other_rows)), ModelErrc::invalid_data);
}

TEST(ArrayData, ItemsMayBeAnyValue) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<NamedArray> arrays;
    arrays.emplace_back("values",
                        std::vector<double>{nan, std::numeric_limits<double>::infinity(), -0.0});
    const ArrayData data(std::move(arrays));
    EXPECT_TRUE(std::isnan(data.find("values")->as<double>()[0]));
}

class ArrayDataInDocument : public ::testing::Test {
protected:
    void SetUp() override { doc.set_owner_enabled(module_a, true); }

    Document doc;
    BusEditor edit{doc};
};

TEST_F(ArrayDataInDocument, ObjectsOfModulesCarryArrayData) {
    NewObject field = module_object(module_a, "field", "U");
    field.data = field_data();
    const ObjectId id = edit.create(field);
    EXPECT_EQ(doc.data<ArrayData>(id), field.data);

    const ObjectId other = edit.create(module_object(module_a, "field", "S"));
    EXPECT_EQ(doc.get(other).data(), nullptr);
    edit.set_data(other, field_data());
    EXPECT_EQ(doc.data<ArrayData>(other)->rows(), 3U);
    edit.set_data(other, nullptr);
    EXPECT_EQ(doc.get(other).data(), nullptr);
}

TEST_F(ArrayDataInDocument, ObjectsOfModulesCarryNoOtherData) {
    NewObject with_mesh = module_object(module_a, "field");
    with_mesh.data = small_mesh();
    EXPECT_MODEL_ERROR(edit.create(with_mesh), ModelErrc::invalid_data);

    const ObjectId id = edit.create(module_object(module_a, "field"));
    EXPECT_MODEL_ERROR(edit.set_data(id, small_mesh()), ModelErrc::invalid_data);
}

TEST_F(ArrayDataInDocument, TypesOfTheCoreKeepTheirOwnData) {
    EXPECT_MODEL_ERROR(edit.create(mesh_object("part", nullptr)), ModelErrc::invalid_data);
    NewObject mesh = mesh_object("part", small_mesh());
    const ObjectId mesh_id = edit.create(mesh);
    EXPECT_MODEL_ERROR(edit.set_data(mesh_id, field_data()), ModelErrc::invalid_data);

    NewObject set = node_set_object(mesh_id, "fixed", nullptr);
    set.data = field_data();
    EXPECT_MODEL_ERROR(edit.create(set), ModelErrc::invalid_data);
}

TEST_F(ArrayDataInDocument, OpaqueObjectKeepsItsData) {
    NewObject field = module_object(module_a, "field", "U");
    field.data = field_data();
    const ObjectId id = edit.create(field);
    doc.set_owner_enabled(module_a, false);

    EXPECT_TRUE(doc.is_opaque(id));
    EXPECT_EQ(doc.get(id).data(), field.data);
    EXPECT_MODEL_ERROR(edit.set_data(id, nullptr), ModelErrc::opaque_object);
    EXPECT_EQ(doc.get(id).data(), field.data);
}

TEST_F(ArrayDataInDocument, UndoPutsTheDataBack) {
    const ObjectId id = edit.create(module_object(module_a, "field", "U"));
    const auto data = field_data();
    edit.set_data(id, data);
    edit.set_data(id, nullptr);
    ASSERT_TRUE(edit.bus().undo());
    EXPECT_EQ(doc.get(id).data(), data);
    ASSERT_TRUE(edit.bus().undo());
    EXPECT_EQ(doc.get(id).data(), nullptr);
}

}  // namespace
