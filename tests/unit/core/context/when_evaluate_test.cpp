#include "fem/core/context/value.h"
#include "fem/core/context/when.h"

#include "context_test_support.h"
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace {

using fem::context::ContextValue;
using fem::context::WhenExpression;
using fem::context::testing::utf8;
using List = ContextValue::List;

/// Values of keys for evaluating expressions without `ContextKeys`.
class Values {
public:
    Values() = default;
    Values(std::initializer_list<std::pair<const std::string, ContextValue>> values)
        : values_(values) {}

    void set(const std::string& key, ContextValue value) { values_[key] = std::move(value); }

    [[nodiscard]] bool eval(std::string_view text) const {
        return WhenExpression::parse(text).evaluate(
            [this](std::string_view key) -> const ContextValue& {
                const auto it = values_.find(key);
                return it == values_.end() ? none_ : it->second;
            });
    }

private:
    std::map<std::string, ContextValue, std::less<>> values_;
    ContextValue none_;
};

TEST(WhenEvaluate, KeyAloneIsItsTruthiness) {
    const Values values{
        {"yes", true},        {"no", false}, {"zero", 0},          {"one", 1},
        {"negative", -0.5},   {"empty", ""}, {"text", "x"},        {"text_false", "false"},
        {"no_items", List{}}, {"none", {}},  {"items", List{"a"}},
    };
    EXPECT_TRUE(values.eval("yes"));
    EXPECT_FALSE(values.eval("no"));
    EXPECT_FALSE(values.eval("zero"));
    EXPECT_TRUE(values.eval("one"));
    EXPECT_TRUE(values.eval("negative"));
    EXPECT_FALSE(values.eval("empty"));
    EXPECT_TRUE(values.eval("text"));
    EXPECT_TRUE(values.eval("text_false"));
    EXPECT_FALSE(values.eval("no_items"));
    EXPECT_TRUE(values.eval("items"));
    EXPECT_FALSE(values.eval("none"));
    EXPECT_FALSE(values.eval("missing"));
    EXPECT_TRUE(values.eval("!missing"));
}

TEST(WhenEvaluate, ConstantsNeedNoKeys) {
    const Values values;
    EXPECT_TRUE(values.eval("true"));
    EXPECT_FALSE(values.eval("false"));
    EXPECT_FALSE(values.eval("!true"));
    EXPECT_TRUE(values.eval("!!true"));
    EXPECT_FALSE(values.eval("true && false"));
    EXPECT_TRUE(values.eval("false || true"));
}

TEST(WhenEvaluate, LogicFollowsPrecedenceAndParentheses) {
    for (int bits = 0; bits < 8; ++bits) {
        const bool a = (bits & 1) != 0;
        const bool b = (bits & 2) != 0;
        const bool c = (bits & 4) != 0;
        const Values values{{"a", a}, {"b", b}, {"c", c}};
        SCOPED_TRACE(testing::Message() << "a=" << a << " b=" << b << " c=" << c);
        EXPECT_EQ(values.eval("a && b"), a && b);
        EXPECT_EQ(values.eval("a || b"), a || b);
        EXPECT_EQ(values.eval("a || b && c"), a || (b && c));
        EXPECT_EQ(values.eval("a && b || c"), (a && b) || c);
        EXPECT_EQ(values.eval("(a || b) && c"), (a || b) && c);
        EXPECT_EQ(values.eval("a && (b || c)"), a && (b || c));
        EXPECT_EQ(values.eval("!a && b"), !a && b);
        EXPECT_EQ(values.eval("!(a && b)"), !(a && b));
        EXPECT_EQ(values.eval("!!a"), a);
        EXPECT_EQ(values.eval("a && b && c"), a && b && c);
        EXPECT_EQ(values.eval("a || b || c"), a || b || c);
    }
}

TEST(WhenEvaluate, EqualityComparesValuesOfTheSameKind) {
    const Values values{
        {"stage", "mesh"}, {"count", 2},  {"ratio", 0.5},          {"flag", true},
        {"off", false},    {"text", "2"}, {"items", List{"mesh"}}, {"name", utf8(u8"сетка")},
    };
    EXPECT_TRUE(values.eval("stage == 'mesh'"));
    EXPECT_FALSE(values.eval("stage == 'Mesh'"));
    EXPECT_FALSE(values.eval("stage == 'mesh '"));
    EXPECT_TRUE(values.eval("stage != 'solve'"));
    EXPECT_TRUE(values.eval(utf8(u8"name == 'сетка'")));

    EXPECT_TRUE(values.eval("count == 2"));
    EXPECT_TRUE(values.eval("count == 2.0"));
    EXPECT_TRUE(values.eval("count == 2e0"));
    EXPECT_TRUE(values.eval("count == 002"));
    EXPECT_FALSE(values.eval("count == 2.5"));
    EXPECT_TRUE(values.eval("ratio == 0.5"));
    EXPECT_TRUE(values.eval("ratio == 5e-1"));
    EXPECT_FALSE(values.eval("ratio == -0.5"));

    EXPECT_TRUE(values.eval("flag == true"));
    EXPECT_FALSE(values.eval("flag == false"));
    EXPECT_TRUE(values.eval("off == false"));
    EXPECT_TRUE(values.eval("flag != false"));

    // Different kinds are never equal.
    EXPECT_FALSE(values.eval("text == 2"));
    EXPECT_FALSE(values.eval("count == '2'"));
    EXPECT_FALSE(values.eval("flag == 'true'"));
    EXPECT_FALSE(values.eval("flag == 1"));
    EXPECT_FALSE(values.eval("items == 'mesh'"));
    EXPECT_TRUE(values.eval("items != 'mesh'"));

    // A key without a value equals nothing, not even false or empty text.
    EXPECT_FALSE(values.eval("missing == false"));
    EXPECT_FALSE(values.eval("missing == ''"));
    EXPECT_FALSE(values.eval("missing == 0"));
    EXPECT_TRUE(values.eval("missing != 'mesh'"));
}

TEST(WhenEvaluate, InLooksForTextInAList) {
    const Values values{
        {"type", "face"},     {"other", "vertex"},   {"count", 1}, {"types", List{"face", "edge"}},
        {"no_types", List{}}, {"text", "face edge"},
    };
    EXPECT_TRUE(values.eval("type in types"));
    EXPECT_FALSE(values.eval("other in types"));
    EXPECT_FALSE(values.eval("type in no_types"));
    EXPECT_FALSE(values.eval("type in missing"));
    EXPECT_FALSE(values.eval("missing in types"));
    // Only lists contain anything, and they contain only text.
    EXPECT_FALSE(values.eval("type in text"));
    EXPECT_FALSE(values.eval("count in types"));
    EXPECT_TRUE(values.eval("!(other in types)"));
}

TEST(WhenEvaluate, KeysOfTheManifestExample) {
    // docs/architecture.md §4.1, with the names of a demo solver.
    Values values{{"model.hasMesh", false}, {"solver.demo.available", true}, {"stage", "mesh"}};
    const std::string menu = "model.hasMesh && solver.demo.available";
    const std::string toolbar = "stage == 'solve'";
    const std::string panel = "job.running && job.solver == 'demo'";
    EXPECT_FALSE(values.eval(menu));
    EXPECT_FALSE(values.eval(toolbar));
    EXPECT_FALSE(values.eval(panel));

    values.set("model.hasMesh", true);
    values.set("stage", "solve");
    values.set("job.running", true);
    values.set("job.solver", "demo");
    EXPECT_TRUE(values.eval(menu));
    EXPECT_TRUE(values.eval(toolbar));
    EXPECT_TRUE(values.eval(panel));
}

}  // namespace
