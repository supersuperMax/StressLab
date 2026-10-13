#include "fem/core/context/value.h"
#include "fem/core/context/when.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fem::context::ContextValue;
using fem::context::UnmetCondition;
using fem::context::WhenExpression;
using Kind = UnmetCondition::Kind;

class Unmet : public ::testing::Test {
protected:
    [[nodiscard]] std::vector<UnmetCondition> unmet(std::string_view text) const {
        return WhenExpression::parse(text).unmet(
            [this](std::string_view key) -> const ContextValue& {
                const auto it = values_.find(key);
                return it == values_.end() ? none_ : it->second;
            });
    }
    /// The texts of the unmet conditions.
    [[nodiscard]] std::vector<std::string> texts(std::string_view text) const {
        std::vector<std::string> out;
        for (const UnmetCondition& condition : unmet(text)) {
            out.push_back(condition.text);
        }
        return out;
    }

    std::map<std::string, ContextValue, std::less<>> values_{
        {"yes", true}, {"no", false}, {"stage", "mesh"}, {"items", ContextValue::List{"a"}}};
    ContextValue none_;
};

TEST_F(Unmet, ATrueExpressionHasNone) {
    EXPECT_TRUE(unmet("yes").empty());
    EXPECT_TRUE(unmet("yes && stage == 'mesh'").empty());
    EXPECT_TRUE(unmet("no || yes").empty());
    EXPECT_TRUE(unmet("!no").empty());
    EXPECT_TRUE(unmet("true").empty());
}

TEST_F(Unmet, AKeyAloneNamesTheKey) {
    const std::vector<UnmetCondition> conditions = unmet("missing");
    ASSERT_EQ(conditions.size(), 1U);
    EXPECT_EQ(conditions[0].kind, Kind::key);
    EXPECT_EQ(conditions[0].key, "missing");
    EXPECT_EQ(conditions[0].text, "missing");
}

TEST_F(Unmet, ANegatedKeyNamesTheKey) {
    const std::vector<UnmetCondition> conditions = unmet("!yes");
    ASSERT_EQ(conditions.size(), 1U);
    EXPECT_EQ(conditions[0].kind, Kind::not_key);
    EXPECT_EQ(conditions[0].key, "yes");
    EXPECT_EQ(conditions[0].text, "!yes");
}

TEST_F(Unmet, AComparisonNamesTheKeyAndTheValue) {
    const std::vector<UnmetCondition> equals = unmet("stage == 'solve'");
    ASSERT_EQ(equals.size(), 1U);
    EXPECT_EQ(equals[0].kind, Kind::equals);
    EXPECT_EQ(equals[0].key, "stage");
    EXPECT_EQ(equals[0].literal, ContextValue("solve"));
    EXPECT_EQ(equals[0].text, "stage == 'solve'");

    const std::vector<UnmetCondition> differs = unmet("stage != 'mesh'");
    ASSERT_EQ(differs.size(), 1U);
    EXPECT_EQ(differs[0].kind, Kind::not_equals);
    EXPECT_EQ(differs[0].literal, ContextValue("mesh"));

    // A negated comparison is the opposite comparison, written as it was.
    const std::vector<UnmetCondition> negated = unmet("!(stage == 'mesh')");
    ASSERT_EQ(negated.size(), 1U);
    EXPECT_EQ(negated[0].kind, Kind::not_equals);
    EXPECT_EQ(negated[0].key, "stage");
    EXPECT_EQ(negated[0].text, "!(stage == 'mesh')");
}

TEST_F(Unmet, AndListsOnlyTheFalseOperands) {
    EXPECT_EQ(texts("yes && no && stage == 'solve' && stage == 'mesh'"),
              (std::vector<std::string>{"no", "stage == 'solve'"}));
}

TEST_F(Unmet, OrIsOneConditionAsAWhole) {
    // Any one of them would do: listing them apart would read as "all of these".
    const std::vector<UnmetCondition> conditions = unmet("no || missing");
    ASSERT_EQ(conditions.size(), 1U);
    EXPECT_EQ(conditions[0].kind, Kind::other);
    EXPECT_EQ(conditions[0].key, "");
    EXPECT_EQ(conditions[0].text, "no || missing");
    EXPECT_EQ(texts("yes && (no || missing) && no"),
              (std::vector<std::string>{"no || missing", "no"}));
}

TEST_F(Unmet, OtherConditionsAreWhole) {
    EXPECT_EQ(texts("stage in items"), (std::vector<std::string>{"stage in items"}));
    EXPECT_EQ(unmet("stage in items")[0].kind, Kind::other);
    EXPECT_EQ(texts("!(yes && !no)"), (std::vector<std::string>{"!(yes && !no)"}));
    EXPECT_EQ(texts("false"), (std::vector<std::string>{"false"}));
    // A double negation is the condition inside it.
    const std::vector<UnmetCondition> twice = unmet("!!no");
    ASSERT_EQ(twice.size(), 1U);
    EXPECT_EQ(twice[0].kind, Kind::key);
    EXPECT_EQ(twice[0].text, "no");
    // An inner && is taken apart too.
    EXPECT_EQ(texts("!!(no && yes && missing)"), (std::vector<std::string>{"no", "missing"}));
}

}  // namespace
