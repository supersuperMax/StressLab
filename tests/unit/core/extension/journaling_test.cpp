// Enabling and disabling modules in the journal (task 1.15): actions of the session that are not
// commands reach the journal as Python that repeats them, without undo steps, and never inside a
// command or a transaction.

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/profile.h"
#include "fem/core/extension/semver.h"
#include "fem/core/model/document.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::command::CommandErrc;
using fem::command::CommandError;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::Journaling;
using fem::extension::Profile;
using fem::extension::Version;
using fem::extension::testing::fixtures;
using fem::extension::testing::Workspace;
using Lines = std::vector<std::string>;

/// The modules of tests/extensions/installed (base <- solver <- post) and the journal of the bus.
class JournalingTest : public ::testing::Test {
protected:
    JournalingTest() {
        (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
        journaling_ =
            ws.bus.on_journal([this](std::string_view entry) { lines.emplace_back(entry); });
    }

    [[nodiscard]] ExtensionState state(std::string_view id) const {
        return ws.manager.get(id).state;
    }

    Workspace ws;
    Lines lines;

private:
    fem::Subscription journaling_;
};

// --- CommandBus::journal_action ------------------------------------------------------------------

TEST(JournalAction, IsAnEntryOfItsOwnWithoutUndo) {
    Workspace ws;
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    (void)ws.manager.enable("test.base");
    Lines lines;
    const auto subscription =
        ws.bus.on_journal([&](std::string_view entry) { lines.emplace_back(entry); });
    (void)ws.bus.execute("model.create", {{"type", "part"}, {"owner", "test.base"}});
    ASSERT_TRUE(ws.bus.undo());
    ASSERT_TRUE(ws.bus.can_redo());

    ws.bus.journal_action("stresslab.extensions.enable(\"x\")");
    EXPECT_EQ(lines.back(), "stresslab.extensions.enable(\"x\")");
    EXPECT_FALSE(ws.bus.can_undo());
    EXPECT_TRUE(ws.bus.can_redo()) << "an action of the session keeps the steps to redo";
}

TEST(JournalAction, IsRefusedInsideATransactionAndForAnEmptyEntry) {
    Workspace ws;
    Lines lines;
    const auto subscription =
        ws.bus.on_journal([&](std::string_view entry) { lines.emplace_back(entry); });
    {
        auto transaction = ws.bus.begin_transaction("t");
        try {
            ws.bus.journal_action("stresslab.extensions.enable(\"x\")");
            ADD_FAILURE() << "an action inside a transaction was journaled";
        } catch (const CommandError& error) {
            EXPECT_EQ(error.code(), CommandErrc::invalid_state);
        }
        transaction.rollback();
    }
    try {
        ws.bus.journal_action("");
        ADD_FAILURE() << "an empty action was journaled";
    } catch (const CommandError& error) {
        EXPECT_EQ(error.code(), CommandErrc::invalid_arguments);
    }
    EXPECT_TRUE(lines.empty());
}

TEST(JournalAction, IsRefusedInsideACommand) {
    Workspace ws;
    bool refused = false;
    ws.bus.register_command({.id = "test.act", .owner = "test.owner"},
                            [&](fem::command::CommandContext&) -> fem::command::CommandResult {
                                try {
                                    ws.bus.journal_action("x()");
                                } catch (const CommandError& error) {
                                    refused = error.code() == CommandErrc::invalid_state;
                                }
                                return {};
                            });
    (void)ws.bus.execute("test.act");
    EXPECT_TRUE(refused);
}

// --- ExtensionManager ----------------------------------------------------------------------------

TEST_F(JournalingTest, EnablingAndDisablingByTheUserIsJournaled) {
    EXPECT_EQ(ws.manager.enable("test.post", Journaling::on),
              (Lines{"test.base", "test.solver", "test.post"}));
    EXPECT_EQ(ws.manager.disable("test.base", Journaling::on),
              (Lines{"test.post", "test.solver", "test.base"}));
    EXPECT_EQ(lines, (Lines{"stresslab.extensions.enable(\"test.post\")",
                            "stresslab.extensions.disable(\"test.base\")"}));
    EXPECT_FALSE(ws.bus.can_undo()) << "enabling a module is not a step of undo";
}

TEST_F(JournalingTest, TheCallIsJournaledEvenIfNothingChanged) {
    (void)ws.manager.enable("test.base");
    EXPECT_TRUE(ws.manager.enable("test.base", Journaling::on).empty());
    EXPECT_TRUE(ws.manager.disable("test.l10n", Journaling::on).empty());
    EXPECT_EQ(lines, (Lines{"stresslab.extensions.enable(\"test.base\")",
                            "stresslab.extensions.disable(\"test.l10n\")"}));
}

TEST_F(JournalingTest, WhatTheApplicationDoesIsNotJournaled) {
    (void)ws.manager.enable("test.post");
    (void)ws.manager.disable("test.post");
    EXPECT_TRUE(lines.empty());
}

TEST_F(JournalingTest, FailedCallsAreNotJournaled) {
    EXPECT_THROW((void)ws.manager.enable("test.future", Journaling::on), ExtensionError);
    EXPECT_THROW((void)ws.manager.enable("test.nothing", Journaling::on), ExtensionError);
    EXPECT_THROW((void)ws.manager.disable("test.nothing", Journaling::on), ExtensionError);
    EXPECT_TRUE(lines.empty());
}

TEST_F(JournalingTest, ModulesAreNotSwitchedInsideATransaction) {
    (void)ws.manager.enable("test.solver");
    {
        auto transaction = ws.bus.begin_transaction("t");
        for (const auto& call : {+[](fem::extension::ExtensionManager& m) {
                                     (void)m.enable("test.l10n", Journaling::on);
                                 },
                                 +[](fem::extension::ExtensionManager& m) {
                                     (void)m.disable("test.solver", Journaling::on);
                                 },
                                 +[](fem::extension::ExtensionManager& m) {
                                     (void)m.apply_profile(Profile{.name = "p", .extensions = {}},
                                                           Journaling::on);
                                 }}) {
            try {
                call(ws.manager);
                ADD_FAILURE() << "a module was switched inside a transaction";
            } catch (const ExtensionError& error) {
                EXPECT_EQ(error.code(), ExtensionErrc::invalid_state);
            }
        }
        transaction.rollback();
    }
    EXPECT_EQ(state("test.l10n"), ExtensionState::disabled);
    EXPECT_EQ(state("test.solver"), ExtensionState::enabled);
    EXPECT_TRUE(lines.empty());
}

TEST_F(JournalingTest, AProfileIsJournaledAsWhatItChanged) {
    (void)ws.manager.enable("test.post");
    const Profile profile{.name = "Только база",
                          .extensions = {{"test.base", Version::parse("1.2.0")},
                                         {"test.l10n", Version::parse("1.0.0")}}};
    (void)ws.manager.apply_profile(profile, Journaling::on);
    EXPECT_EQ(lines, (Lines{"stresslab.extensions.disable(\"test.post\")",
                            "stresslab.extensions.disable(\"test.solver\")",
                            "stresslab.extensions.enable(\"test.l10n\")"}));
}

}  // namespace
