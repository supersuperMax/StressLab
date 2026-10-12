#include "fem/core/context/context_keys.h"

#include "fem/core/context/error.h"
#include "fem/core/context/value.h"
#include "fem/core/context/when.h"

#include "context_test_support.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using fem::context::ContextErrc;
using fem::context::ContextKeys;
using fem::context::ContextValue;
using fem::context::KeySpec;
using fem::context::WhenExpression;
using fem::context::testing::utf8;
using Kind = ContextValue::Kind;
using List = ContextValue::List;

constexpr const char* module_a = "test.module-a";
constexpr const char* module_b = "test.module-b";

TEST(ContextValue, KindsAndAccessors) {
    EXPECT_EQ(ContextValue().kind(), Kind::none);
    EXPECT_TRUE(ContextValue(nullptr).is_none());
    EXPECT_EQ(ContextValue(true).kind(), Kind::boolean);
    EXPECT_EQ(ContextValue(3).kind(), Kind::number);
    EXPECT_EQ(ContextValue(std::uint64_t{7}).as_number(), 7.0);
    EXPECT_EQ(ContextValue(2.5).as_number(), 2.5);
    // A C string is text, not a boolean.
    EXPECT_EQ(ContextValue("mesh").kind(), Kind::string);
    EXPECT_EQ(ContextValue("mesh").as_string(), "mesh");
    EXPECT_EQ(ContextValue(std::string("x")).as_string(), "x");
    EXPECT_EQ(ContextValue(List{"a", "b"}).as_list(), (List{"a", "b"}));

    EXPECT_CONTEXT_ERROR((void)ContextValue("x").as_boolean(), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR((void)ContextValue(true).as_number(), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR((void)ContextValue().as_string(), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR((void)ContextValue(1).as_list(), ContextErrc::invalid_value);

    EXPECT_EQ(ContextValue(2), ContextValue(2.0));
    EXPECT_NE(ContextValue(1), ContextValue(true));
    EXPECT_NE(ContextValue("2"), ContextValue(2));
    EXPECT_EQ(fem::context::to_string(Kind::list), "list");
}

TEST(ContextKeys, DeclaredKeyHasItsInitialValue) {
    ContextKeys keys;
    EXPECT_TRUE(keys.declare({.name = "stage", .owner = module_a, .type = Kind::string}));
    EXPECT_TRUE(keys.declare(
        {.name = "app.ready", .owner = module_a, .type = Kind::boolean, .initial = false}));

    ASSERT_NE(keys.find("stage"), nullptr);
    EXPECT_EQ(keys.find("stage")->owner, module_a);
    EXPECT_EQ(keys.find("missing"), nullptr);
    EXPECT_TRUE(keys.value("stage").is_none());
    EXPECT_EQ(keys.value("app.ready"), ContextValue(false));
    EXPECT_TRUE(keys.value("missing").is_none());
    EXPECT_EQ(keys.keys(), (std::vector<std::string>{"app.ready", "stage"}));
}

TEST(ContextKeys, SameDeclarationAgainChangesNothing) {
    ContextKeys keys;
    const KeySpec spec{.name = "stage", .owner = module_a, .type = Kind::string, .initial = "mesh"};
    EXPECT_TRUE(keys.declare(spec));
    keys.set("stage", "solve");
    EXPECT_FALSE(keys.declare(spec));
    EXPECT_EQ(keys.value("stage"), ContextValue("solve"));

    KeySpec other_owner = spec;
    other_owner.owner = module_b;
    EXPECT_CONTEXT_ERROR(keys.declare(other_owner), ContextErrc::duplicate_key);
    KeySpec other_type = spec;
    other_type.type = Kind::number;
    other_type.initial = {};
    EXPECT_CONTEXT_ERROR(keys.declare(other_type), ContextErrc::duplicate_key);
    KeySpec other_initial = spec;
    other_initial.initial = "solve";
    EXPECT_CONTEXT_ERROR(keys.declare(other_initial), ContextErrc::duplicate_key);
}

TEST(ContextKeys, BadDeclarationsAreRefused) {
    ContextKeys keys;
    for (const char* name : {"", "a.", "1a", "a b", "true", "in"}) {
        EXPECT_CONTEXT_ERROR(keys.declare({.name = name, .owner = module_a}),
                             ContextErrc::invalid_key);
    }
    EXPECT_CONTEXT_ERROR(keys.declare({.name = "a", .owner = ""}), ContextErrc::invalid_spec);
    EXPECT_CONTEXT_ERROR(keys.declare({.name = "a", .owner = module_a, .type = Kind::none}),
                         ContextErrc::invalid_spec);
    EXPECT_CONTEXT_ERROR(
        keys.declare({.name = "a", .owner = module_a, .type = Kind::boolean, .initial = "yes"}),
        ContextErrc::invalid_spec);
    EXPECT_CONTEXT_ERROR(keys.declare({.name = "a",
                                       .owner = module_a,
                                       .type = Kind::number,
                                       .initial = std::numeric_limits<double>::quiet_NaN()}),
                         ContextErrc::invalid_spec);
    EXPECT_TRUE(keys.keys().empty());
}

TEST(ContextKeys, SetChecksTheKeyAndTheValue) {
    ContextKeys keys;
    keys.declare({.name = "flag", .owner = module_a, .type = Kind::boolean});
    keys.declare({.name = "count", .owner = module_a, .type = Kind::number});
    keys.declare({.name = "stage", .owner = module_a, .type = Kind::string});
    keys.declare({.name = "types", .owner = module_a, .type = Kind::list});

    keys.set("flag", true);
    keys.set("count", 3);
    keys.set("stage", utf8(u8"сетка"));
    keys.set("types", List{"face"});
    EXPECT_EQ(keys.value("flag"), ContextValue(true));
    EXPECT_EQ(keys.value("count"), ContextValue(3));
    EXPECT_EQ(keys.value("stage"), ContextValue(utf8(u8"сетка")));
    EXPECT_EQ(keys.value("types"), ContextValue(List{"face"}));

    // None clears the value.
    keys.set("flag", {});
    EXPECT_TRUE(keys.value("flag").is_none());

    EXPECT_CONTEXT_ERROR(keys.set("missing", true), ContextErrc::unknown_key);
    EXPECT_CONTEXT_ERROR(keys.set("flag", 1), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set("stage", true), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set("count", std::numeric_limits<double>::infinity()),
                         ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set("count", std::numeric_limits<double>::quiet_NaN()),
                         ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set("stage", "bad \xFF"), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set("types", List{"ok", "bad \xC0\x80"}), ContextErrc::invalid_value);
    EXPECT_EQ(keys.value("count"), ContextValue(3));
    EXPECT_EQ(keys.value("stage"), ContextValue(utf8(u8"сетка")));
}

TEST(ContextKeys, SetOfSeveralKeysIsAllOrNothing) {
    ContextKeys keys;
    keys.declare({.name = "a", .owner = module_a, .type = Kind::boolean, .initial = false});
    keys.declare({.name = "b", .owner = module_a, .type = Kind::number, .initial = 0});

    EXPECT_CONTEXT_ERROR(keys.set({{"a", true}, {"b", "two"}}), ContextErrc::invalid_value);
    EXPECT_CONTEXT_ERROR(keys.set({{"a", true}, {"missing", 1}}), ContextErrc::unknown_key);
    EXPECT_EQ(keys.value("a"), ContextValue(false));
    EXPECT_EQ(keys.value("b"), ContextValue(0));

    keys.set({{"a", true}, {"b", 1}, {"b", 2}});
    EXPECT_EQ(keys.value("a"), ContextValue(true));
    EXPECT_EQ(keys.value("b"), ContextValue(2));
}

TEST(ContextKeys, RemoveOwnerTakesItsKeysAway) {
    ContextKeys keys;
    keys.declare({.name = "a.one", .owner = module_a, .initial = true});
    keys.declare({.name = "a.two", .owner = module_a, .type = Kind::string});
    keys.declare({.name = "b.one", .owner = module_b, .initial = true});

    EXPECT_EQ(keys.remove_owner(module_a), 2U);
    EXPECT_EQ(keys.keys(), (std::vector<std::string>{"b.one"}));
    EXPECT_TRUE(keys.value("a.one").is_none());
    EXPECT_CONTEXT_ERROR(keys.set("a.one", true), ContextErrc::unknown_key);
    EXPECT_EQ(keys.value("b.one"), ContextValue(true));
    EXPECT_EQ(keys.remove_owner(module_a), 0U);
    EXPECT_EQ(keys.remove_owner("test.unknown"), 0U);

    // The module comes back, maybe in another version.
    EXPECT_TRUE(keys.declare({.name = "a.one", .owner = module_a, .type = Kind::number}));
    EXPECT_TRUE(keys.value("a.one").is_none());

    EXPECT_CONTEXT_ERROR(keys.remove_owner("fem.core"), ContextErrc::invalid_spec);
}

TEST(ContextKeys, EvaluatesExpressionsOnTheCurrentValues) {
    ContextKeys keys;
    keys.declare({.name = "stage", .owner = module_a, .type = Kind::string, .initial = "mesh"});
    keys.declare({.name = "model.hasMesh", .owner = module_a, .initial = false});
    const WhenExpression expression =
        WhenExpression::parse("model.hasMesh && stage == 'solve' && !unknown.key");

    EXPECT_FALSE(keys.evaluate(expression));
    keys.set("model.hasMesh", true);
    EXPECT_FALSE(keys.evaluate(expression));
    keys.set("stage", "solve");
    EXPECT_TRUE(keys.evaluate(expression));
}

}  // namespace
