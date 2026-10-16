// The history of a project (task 2.3): versions, variants, labels, changes put aside, automatic
// versions, restoring a version and one object, the journal of each version.

#include "fem/storage/history.h"

#include "fem/core/command/error.h"
#include "fem/core/command/journal.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/io/files.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/error.h"
#include "fem/storage/session.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using command::Value;
using model::ObjectId;
namespace mc = command::model_commands;

constexpr const char* module = "fem.demo";

std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

#define EXPECT_PROJECT_ERROR(statement, expected)                                    \
    do {                                                                             \
        try {                                                                        \
            statement;                                                               \
            ADD_FAILURE() << "Expected ProjectError " << to_string(expected);        \
        } catch (const ProjectError& error) {                                        \
            EXPECT_EQ(to_string(error.code()), to_string(expected)) << error.what(); \
        }                                                                            \
    } while (false)

class StorageHistory : public ::testing::Test {
protected:
    StorageHistory()
        : dir("history"),
          session(doc.bus(),
                  [](std::string_view id) { return id == module ? std::string("1.5.0") : ""; }),
          history(session.history()),
          journal_(doc.bus().on_journal(
              [this](std::string_view entry) { journal.emplace_back(entry); })) {
        doc.document.set_owner_enabled(module, true);
    }

    command::CommandBus& bus() { return doc.bus(); }

    ObjectId add(const std::string& type, const std::string& name,
                 std::vector<ObjectId> dependencies = {}) {
        Value::List targets;
        for (const ObjectId id : dependencies) {
            targets.emplace_back(id);
        }
        return ObjectId(static_cast<std::uint64_t>(
            bus()
                .execute(mc::create, {{"type", type},
                                      {"owner", module},
                                      {"name", name},
                                      {"dependencies", std::move(targets)}})
                .value.as_integer()));
    }

    void set(ObjectId id, double thickness) {
        bus().execute(mc::set_property,
                      {{"object", id}, {"key", "thickness"}, {"value", thickness}});
    }

    double thickness(ObjectId id) const {
        return doc.document.get(id).properties().at("thickness").get<double>();
    }

    fs::path folder() const { return dir.path() / fs::path(u8"крыло.slproj"); }

    /// A project saved to `folder()` with one object, "skin" of thickness 2.
    ObjectId saved_project() {
        const ObjectId skin = add("section", "skin");
        set(skin, 2.0);
        session.save_as(folder());
        return skin;
    }

    /// The journal since the last call.
    std::vector<std::string> take_journal() {
        std::vector<std::string> result;
        result.swap(journal);
        return result;
    }

    TempDir dir;
    TestDocument doc;
    ProjectSession session;
    ProjectHistory& history;
    std::vector<std::string> journal;

private:
    Subscription journal_;
};

}  // namespace

// --- Versions ------------------------------------------------------------------------------------

TEST_F(StorageHistory, ProjectWithoutVersionsIsOnMain) {
    EXPECT_EQ(history.current_branch(), std::optional<std::string>("main"));
    EXPECT_FALSE(history.current_version().has_value());
    EXPECT_TRUE(history.log().empty());
    EXPECT_TRUE(history.branches().empty());
    EXPECT_FALSE(history.has_changes());
    (void)add("section", "skin");
    EXPECT_TRUE(history.has_changes());
}

TEST_F(StorageHistory, CommitSavesAndMakesAVersion) {
    const ObjectId skin = saved_project();
    set(skin, 3.0);
    ASSERT_TRUE(session.is_modified());
    take_journal();

    const std::string id = history.commit(utf8(u8"Толщина обшивки 3 мм"));

    EXPECT_FALSE(session.is_modified());
    EXPECT_FALSE(history.has_changes());
    EXPECT_EQ(history.current_version(), std::optional<std::string>(id));
    EXPECT_EQ(history.current_branch(), std::optional<std::string>("main"));
    EXPECT_EQ(history.branches().at("main"), id);
    const std::vector<VersionInfo> log = history.log();
    ASSERT_EQ(log.size(), 1U);
    EXPECT_EQ(log[0].summary, utf8(u8"Толщина обшивки 3 мм"));
    EXPECT_EQ(log[0].kind, VersionKind::manual);
    EXPECT_EQ(take_journal(), std::vector<std::string>{
                                  utf8(u8"stresslab.history.commit(\"Толщина обшивки 3 мм\")")});

    // The folder holds the version: opening it gives the committed model.
    TestDocument other;
    ProjectSession reopened(other.bus());
    reopened.open(folder());
    EXPECT_EQ(other.document.get(skin).properties().at("thickness").get<double>(), 3.0);
    EXPECT_EQ(reopened.history().current_version(), std::optional<std::string>(id));
    EXPECT_FALSE(reopened.history().has_changes());
}

TEST_F(StorageHistory, CommitIsRefusedWhenItCannotBeMade) {
    (void)add("section", "skin");
    EXPECT_PROJECT_ERROR((void)history.commit("No folder"), ProjectErrc::no_folder);
    session.save_as(folder());
    EXPECT_PROJECT_ERROR((void)history.commit("  \n"), ProjectErrc::invalid_name);
    (void)history.commit("First");
    EXPECT_PROJECT_ERROR((void)history.commit("Again"), ProjectErrc::nothing_to_commit);
    {
        command::Transaction transaction = bus().begin_transaction("group");
        EXPECT_THROW((void)history.commit("Inside"), command::CommandError);
        transaction.commit();
    }
    EXPECT_EQ(history.log().size(), 1U);
}

TEST_F(StorageHistory, UndoBackToTheVersionIsNoChange) {
    const ObjectId skin = saved_project();
    (void)history.commit("First");
    set(skin, 5.0);
    EXPECT_TRUE(history.has_changes());
    ASSERT_TRUE(bus().undo());
    EXPECT_FALSE(history.has_changes());
}

TEST_F(StorageHistory, NewerModuleVersionIsNoChange) {
    (void)saved_project();
    (void)history.commit("First");
    TestDocument other;
    ProjectSession upgraded(other.bus(), [](std::string_view id) {
        return id == module ? std::string("9.0.0") : std::string();
    });
    upgraded.open(folder());
    EXPECT_FALSE(upgraded.history().has_changes());
}

// --- The journal of a version --------------------------------------------------------------------

TEST_F(StorageHistory, VersionKeepsTheJournalSinceTheVersionBefore) {
    const ObjectId skin = saved_project();
    const std::string first = history.commit("First");
    bus().execute(mc::rename, {{"object", skin}, {"name", "upper skin"}});
    set(skin, 3.0);
    const std::string second = history.commit("Second");

    const std::string text = history.journal(second);
    EXPECT_TRUE(text.starts_with(command::journal_header()));
    EXPECT_NE(text.find("stresslab.cmd(\"model.rename\", object=1, name=\"upper skin\")\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("\"thickness\""), std::string::npos) << text;
    // Commits are not a part of any journal.
    EXPECT_EQ(text.find("stresslab.history.commit"), std::string::npos) << text;
    EXPECT_EQ(history.journal(first).find("upper skin"), std::string::npos);
    // The journal is a script the parser of journals reads.
    EXPECT_NO_THROW((void)command::parse_journal(text));
}

TEST_F(StorageHistory, JournalSinceTheLastVersionOutlivesTheSession) {
    const ObjectId skin = saved_project();
    (void)history.commit("First");
    bus().execute(mc::rename, {{"object", skin}, {"name", "renamed before closing"}});
    session.save();
    EXPECT_TRUE(fs::exists(folder() / ".fem" / "journal.slpy"));
    session.close();

    session.open(folder());
    set(skin, 4.0);
    const std::string second = history.commit("Second");

    const std::string text = history.journal(second);
    EXPECT_NE(text.find("renamed before closing"), std::string::npos) << text;
    EXPECT_NE(text.find("\"thickness\""), std::string::npos) << text;
    EXPECT_FALSE(fs::exists(folder() / ".fem" / "journal.slpy"));
}

// --- Variants and labels -------------------------------------------------------------------------

TEST_F(StorageHistory, VariantsHoldTheirOwnModels) {
    const ObjectId skin = saved_project();
    const std::string base = history.commit("Base");
    const std::string variant = utf8(u8"Композитная обшивка");
    history.branch(variant);
    EXPECT_EQ(history.current_branch(), std::optional<std::string>("main"));

    history.checkout(variant);
    EXPECT_EQ(history.current_branch(), std::optional<std::string>(variant));
    set(skin, 3.0);
    const std::string composite = history.commit("Composite skin");

    history.checkout("main");
    EXPECT_EQ(thickness(skin), 2.0);
    EXPECT_EQ(history.current_version(), std::optional<std::string>(base));
    EXPECT_FALSE(session.is_modified());
    history.checkout(variant);
    EXPECT_EQ(thickness(skin), 3.0);
    EXPECT_EQ(history.branches(),
              (std::map<std::string, std::string>{{"main", base}, {variant, composite}}));
    // The folder is the variant.
    TestDocument other;
    ProjectSession reopened(other.bus());
    reopened.open(folder());
    EXPECT_EQ(other.document.get(skin).properties().at("thickness").get<double>(), 3.0);
    EXPECT_EQ(reopened.history().current_branch(), std::optional<std::string>(variant));
}

TEST_F(StorageHistory, CheckoutWithChangesIsRefused) {
    const ObjectId skin = saved_project();
    (void)history.commit("Base");
    history.branch("other");
    set(skin, 7.0);
    EXPECT_PROJECT_ERROR(history.checkout("other"), ProjectErrc::uncommitted_changes);
    EXPECT_EQ(thickness(skin), 7.0);
    EXPECT_PROJECT_ERROR(history.checkout("missing"), ProjectErrc::unknown_version);
    EXPECT_PROJECT_ERROR(history.branch("other"), ProjectErrc::already_exists);
}

TEST_F(StorageHistory, VersionViewedOnItsOwnBecomesAVariantToChangeIt) {
    const ObjectId skin = saved_project();
    const std::string first = history.commit("First");
    set(skin, 3.0);
    (void)history.commit("Second");

    history.checkout(first);
    EXPECT_FALSE(history.current_branch().has_value());
    EXPECT_EQ(history.current_version(), std::optional<std::string>(first));
    EXPECT_EQ(thickness(skin), 2.0);
    set(skin, 9.0);
    EXPECT_PROJECT_ERROR((void)history.commit("Lost?"), ProjectErrc::detached);
    EXPECT_PROJECT_ERROR((void)history.snapshot("Lost?"), ProjectErrc::detached);
    ASSERT_TRUE(bus().undo());

    history.branch("from first", first);
    history.checkout("from first");
    set(skin, 9.0);
    const std::string third = history.commit("Third");
    EXPECT_EQ(history.info(third).parents, std::vector<std::string>{first});
}

TEST_F(StorageHistory, LabelsMarkVersions) {
    (void)saved_project();
    const std::string first = history.commit("First");
    const std::string label = utf8(u8"Отправлено на согласование 12.10");
    history.tag(label);
    EXPECT_EQ(history.tags().at(label), first);
    EXPECT_EQ(history.resolve(label), first);
    EXPECT_PROJECT_ERROR(history.tag(label), ProjectErrc::already_exists);
    history.checkout(label);
    EXPECT_FALSE(history.current_branch().has_value());
}

// --- Changes put aside ---------------------------------------------------------------------------

TEST_F(StorageHistory, StashPutsChangesAsideAndBringsThemBack) {
    const ObjectId skin = saved_project();
    (void)history.commit("Base");
    EXPECT_PROJECT_ERROR(history.stash(), ProjectErrc::nothing_to_stash);
    bus().execute(mc::rename, {{"object", skin}, {"name", "aside"}});

    history.stash("Work in progress");
    EXPECT_EQ(doc.document.get(skin).name(), "skin");
    EXPECT_FALSE(history.has_changes());
    const std::vector<VersionInfo> stashes = history.stashes();
    ASSERT_EQ(stashes.size(), 1U);
    EXPECT_EQ(stashes[0].message, "Work in progress");
    EXPECT_EQ(stashes[0].kind, VersionKind::stash);
    EXPECT_EQ(stashes[0].branch, "main");

    history.stash_pop();
    EXPECT_EQ(doc.document.get(skin).name(), "aside");
    EXPECT_TRUE(history.has_changes());
    EXPECT_TRUE(history.stashes().empty());
    // The journal of the changes came back with them.
    const std::string id = history.commit("Renamed");
    EXPECT_NE(history.journal(id).find("name=\"aside\""), std::string::npos);
}

TEST_F(StorageHistory, StashGoesBackOnlyWhereItWasPutAside) {
    const ObjectId skin = saved_project();
    (void)history.commit("Base");
    set(skin, 3.0);
    history.stash();
    set(skin, 4.0);
    EXPECT_PROJECT_ERROR(history.stash_pop(), ProjectErrc::uncommitted_changes);
    (void)history.commit("Moved on");
    EXPECT_PROJECT_ERROR(history.stash_pop(), ProjectErrc::stash_conflict);
    EXPECT_PROJECT_ERROR(history.stash_pop(1), ProjectErrc::unknown_version);
    history.stash_drop();
    EXPECT_TRUE(history.stashes().empty());
}

// --- Restoring -----------------------------------------------------------------------------------

TEST_F(StorageHistory, RestoringAVersionLosesNothing) {
    const ObjectId skin = saved_project();
    const std::string first = history.commit("Two millimetres");
    set(skin, 3.0);
    const ObjectId rib = add("rib", "rib 7");
    const std::string second = history.commit("Three millimetres and a rib");
    set(skin, 4.0);  // Not in the history yet.
    take_journal();

    const std::string restored = history.restore_version(first);

    EXPECT_EQ(thickness(skin), 2.0);
    EXPECT_FALSE(doc.document.contains(rib));
    EXPECT_FALSE(history.has_changes());
    const std::vector<VersionInfo> log = history.log();
    ASSERT_EQ(log.size(), 4U);
    EXPECT_EQ(log[0].id, restored);
    EXPECT_EQ(log[0].kind, VersionKind::restore);
    EXPECT_EQ(log[0].restored, first);
    EXPECT_EQ(log[1].kind, VersionKind::automatic);
    EXPECT_EQ(log[2].id, second);
    EXPECT_EQ(log[3].id, first);
    // The automatic version kept the change that was not in the history.
    TestDocument viewer;
    ProjectSession viewing(viewer.bus());
    viewing.open(folder());
    EXPECT_EQ(viewing.history().info("main~1").kind, VersionKind::automatic);
    // Ids of the newer objects are not given out again.
    EXPECT_GT(doc.document.next_id(), rib.value());
    EXPECT_NE(add("rib", "new"), rib);
    EXPECT_EQ(take_journal().front(), "stresslab.history.restore_version(\"" + first + "\")");
    EXPECT_NE(history.journal(restored).find("stresslab.history.restore_version"),
              std::string::npos);
}

TEST_F(StorageHistory, RestoringOneObjectIsOneStepOfUndo) {
    const ObjectId material = add("material", "steel");
    set(material, 1.0);
    const ObjectId skin = add("section", "skin", {material});
    set(skin, 2.0);
    session.save_as(folder());
    (void)history.commit("First");
    set(material, 5.0);
    bus().execute(mc::rename, {{"object", skin}, {"name", "renamed"}});
    bus().execute(mc::remove, {{"object", material}});
    (void)history.commit("Second");
    take_journal();

    // The removed object comes back with its id, and its dependent sees it again.
    bus().execute(std::string(ProjectHistory::restore_object_command),
                  {{"object", material}, {"version", "main~1"}});
    EXPECT_EQ(thickness(material), 1.0);
    EXPECT_TRUE(doc.document.missing_dependencies(skin).empty());
    EXPECT_EQ(take_journal(), std::vector<std::string>{"stresslab.cmd(\"history.restore_object\", "
                                                       "object=1, version=\"main~1\")"});

    // An object that is there gets its name and properties back.
    bus().execute(std::string(ProjectHistory::restore_object_command),
                  {{"object", skin}, {"version", "main~1"}});
    EXPECT_EQ(doc.document.get(skin).name(), "skin");

    ASSERT_TRUE(bus().undo());
    EXPECT_EQ(doc.document.get(skin).name(), "renamed");
    ASSERT_TRUE(bus().undo());
    EXPECT_FALSE(doc.document.contains(material));
    ASSERT_TRUE(bus().redo());
    EXPECT_EQ(thickness(material), 1.0);

    EXPECT_THROW(bus().execute(std::string(ProjectHistory::restore_object_command),
                               {{"object", 99}, {"version", "main"}}),
                 model::ModelError);
    EXPECT_PROJECT_ERROR(bus().execute(std::string(ProjectHistory::restore_object_command),
                                       {{"object", skin}, {"version", "nowhere"}}),
                         ProjectErrc::unknown_version);
}

TEST_F(StorageHistory, RestoringAMeshBringsItsData) {
    const ObjectId mesh = doc.edit.create(model::mesh_object("wing", model::testing::small_mesh()));
    session.save_as(folder());
    (void)history.commit("Fine mesh");
    doc.edit.set_data(mesh, model::testing::tiny_mesh());
    (void)history.commit("Coarse mesh");

    bus().execute(std::string(ProjectHistory::restore_object_command),
                  {{"object", mesh}, {"version", "main~1"}});
    EXPECT_EQ(doc.document.data<model::Mesh>(mesh)->node_count(), 5U);
}

// --- Automatic versions --------------------------------------------------------------------------

TEST_F(StorageHistory, RiskyCommandGetsAnAutomaticVersionBeforeIt) {
    bus().register_command({.id = "test.remesh", .owner = module, .snapshot_before = true},
                           [](command::CommandContext&) { return command::CommandResult{}; });
    const ObjectId skin = saved_project();
    (void)history.commit("First");
    set(skin, 3.0);
    take_journal();

    bus().execute("test.remesh");
    const std::vector<VersionInfo> log = history.log();
    ASSERT_EQ(log.size(), 2U);
    EXPECT_EQ(log[0].kind, VersionKind::automatic);
    EXPECT_EQ(log[0].before, "test.remesh");
    EXPECT_EQ(log[0].summary, "Before test.remesh");
    // The command implies the version: the journal has the command alone.
    EXPECT_EQ(take_journal(), std::vector<std::string>{"stresslab.cmd(\"test.remesh\")"});

    // Nothing changed since: no new version.
    bus().execute("test.remesh");
    EXPECT_EQ(history.log().size(), 2U);
}

TEST_F(StorageHistory, SnapshotReturnsTheVersionTheModelIs) {
    const ObjectId skin = saved_project();
    const std::string first = history.snapshot("Before solving");
    EXPECT_EQ(history.info(first).kind, VersionKind::automatic);
    EXPECT_EQ(history.snapshot("Before solving"), first);
    set(skin, 3.0);
    const std::string second = history.snapshot("Before solving");
    EXPECT_NE(second, first);
    EXPECT_EQ(history.current_version(), std::optional<std::string>(second));
}

TEST_F(StorageHistory, ProjectWithoutFolderGetsNoAutomaticVersion) {
    bus().register_command({.id = "test.remesh", .owner = module, .snapshot_before = true},
                           [](command::CommandContext&) { return command::CommandResult{}; });
    (void)add("section", "skin");
    EXPECT_NO_THROW(bus().execute("test.remesh"));
    EXPECT_TRUE(history.log().empty());
}

// --- Cleaning ------------------------------------------------------------------------------------

TEST_F(StorageHistory, CleaningKeepsTheDataOfEveryVersion) {
    const ObjectId mesh = doc.edit.create(model::mesh_object("wing", model::testing::small_mesh()));
    session.save_as(folder());
    const std::string fine = history.commit("Fine mesh");
    history.tag("fine");
    doc.edit.set_data(mesh, model::testing::tiny_mesh());
    (void)history.commit("Coarse mesh");

    (void)session.clean();

    history.checkout("fine");
    EXPECT_EQ(doc.document.data<model::Mesh>(mesh)->node_count(), 5U);
    history.checkout("main");
    EXPECT_EQ(doc.document.data<model::Mesh>(mesh)->node_count(), 2U);
}

// --- Journal lines -------------------------------------------------------------------------------

TEST_F(StorageHistory, ActionsAreJournaledAsTheyWereGiven) {
    const ObjectId skin = saved_project();
    (void)history.commit("First");
    take_journal();
    history.branch(utf8(u8"Вариант"), "main");
    history.tag("v1");
    history.checkout(utf8(u8"Вариант"));
    set(skin, 3.0);
    history.stash();
    history.stash_pop();
    (void)history.snapshot("Before solving");
    std::vector<std::string> lines = take_journal();
    lines.erase(
        std::remove_if(lines.begin(), lines.end(),
                       [](const std::string& line) { return line.starts_with("stresslab.cmd("); }),
        lines.end());
    EXPECT_EQ(lines, (std::vector<std::string>{
                         utf8(u8"stresslab.history.branch(\"Вариант\", at=\"main\")"),
                         "stresslab.history.tag(\"v1\")",
                         utf8(u8"stresslab.history.checkout(\"Вариант\")"),
                         "stresslab.history.stash()",
                         "stresslab.history.stash_pop()",
                         "stresslab.history.snapshot(\"Before solving\")",
                     }));
}

}  // namespace fem::storage
