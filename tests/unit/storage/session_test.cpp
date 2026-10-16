#include "fem/storage/session.h"

#include "fem/core/command/error.h"
#include "fem/core/io/files.h"
#include "fem/core/model/object.h"
#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

constexpr const char* module = "fem.demo";

class StorageSession : public ::testing::Test {
protected:
    StorageSession()
        : dir("session"),
          session(doc.bus(),
                  [](std::string_view id) { return id == module ? std::string("1.5.0") : ""; }),
          journal_(doc.bus().on_journal(
              [this](std::string_view entry) { journal.emplace_back(entry); })),
          changes_(session.on_changed([this] { ++changes; })) {
        doc.document.set_owner_enabled(module, true);
    }

    ObjectId add_note(const std::string& name) {
        return doc.edit.create(model::testing::module_object(module, "note", name));
    }

    /// The journal since the last call, without the commands of the test editor.
    std::vector<std::string> take_journal() {
        std::vector<std::string> result;
        for (std::string& line : journal) {
            if (!line.starts_with("stresslab.cmd(\"test.edit\"")) {
                result.push_back(std::move(line));
            }
        }
        journal.clear();
        return result;
    }

    TempDir dir;
    TestDocument doc;
    ProjectSession session;
    std::vector<std::string> journal;
    int changes = 0;

private:
    Subscription journal_;
    Subscription changes_;
};

ProjectError project_error(const std::function<void()>& action) {
    try {
        action();
    } catch (const ProjectError& error) {
        return error;
    }
    ADD_FAILURE() << "no ProjectError";
    return ProjectError(ProjectErrc::io, "none");
}

TEST_F(StorageSession, NewProjectHasNoFolder) {
    EXPECT_FALSE(session.folder());
    EXPECT_FALSE(session.is_modified());
    add_note("a");
    EXPECT_TRUE(session.is_modified());
    EXPECT_EQ(changes, 1);
    add_note("b");
    EXPECT_EQ(changes, 1);  // Already modified: no new notice.
    EXPECT_EQ(project_error([&] { session.save(); }).code(), ProjectErrc::no_folder);
    EXPECT_FALSE(session.autosave());
}

TEST_F(StorageSession, SaveAsAddsTheExtensionAndBecomesTheFolder) {
    add_note("a");
    take_journal();
    session.save_as(dir.path() / fs::path(u8"крыло"));
    const fs::path folder = dir.path() / fs::path(u8"крыло.slproj");
    ASSERT_TRUE(session.folder());
    EXPECT_TRUE(fs::equivalent(*session.folder(), folder));
    EXPECT_TRUE(is_project_folder(folder));
    EXPECT_FALSE(session.is_modified());
    EXPECT_EQ(take_journal(), (std::vector<std::string>{"stresslab.project.save_as(\"" +
                                                        io::display(folder) + "\")"}));

    // Versions of the modules come from the lookup.
    const std::string project = io::read_file(folder / "project.json").value();
    EXPECT_NE(project.find("\"fem.demo\": \"1.5.0\""), std::string::npos) << project;

    add_note("b");
    session.save();
    EXPECT_FALSE(session.is_modified());
    EXPECT_EQ(take_journal(), (std::vector<std::string>{"stresslab.project.save()"}));
    EXPECT_EQ(ProjectSession::with_extension("a.slproj"), fs::path("a.slproj"));
    EXPECT_EQ(ProjectSession::with_extension("a.slproj/"), fs::path("a.slproj"));
    EXPECT_EQ(ProjectSession::with_extension("a.txt"), fs::path("a.txt.slproj"));
}

TEST_F(StorageSession, OpenReplacesTheDocument) {
    add_note("saved");
    session.save_as(dir.path() / "p.slproj");
    add_note("unsaved");
    ASSERT_TRUE(doc.bus().can_undo());
    take_journal();

    std::vector<command::ModelChange> events;
    const Subscription listening = doc.bus().on_model_changed(
        [&events](const command::ModelChange& change) { events.push_back(change); });
    session.open(dir.path() / "p.slproj");
    EXPECT_EQ(doc.document.size(), 1U);
    EXPECT_EQ(doc.document.get(ObjectId(1)).name(), "saved");
    EXPECT_EQ(doc.document.next_id(), 2U);
    EXPECT_FALSE(session.is_modified());
    EXPECT_FALSE(doc.bus().can_undo());  // Steps of the old contents are gone.
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].cause, command::ModelChange::Cause::load);
    EXPECT_EQ(events[0].summary.updated, (std::vector<ObjectId>{ObjectId(1)}));
    EXPECT_EQ(events[0].summary.removed, (std::vector<ObjectId>{ObjectId(2)}));
    EXPECT_EQ(take_journal(),
              (std::vector<std::string>{"stresslab.project.open(\"" +
                                        io::display(dir.path() / "p.slproj") + "\")"}));

    session.close();
    EXPECT_EQ(doc.document.size(), 0U);
    EXPECT_FALSE(session.folder());
    EXPECT_EQ(take_journal(), (std::vector<std::string>{"stresslab.project.close()"}));
}

TEST_F(StorageSession, AutosaveOnlyWhenThereIsSomethingToSave) {
    session.save_as(dir.path() / "p.slproj");
    take_journal();
    EXPECT_FALSE(session.autosave());
    add_note("a");
    EXPECT_TRUE(session.autosave());
    EXPECT_FALSE(session.is_modified());
    EXPECT_TRUE(take_journal().empty());  // Autosave is not a step of the user.
    add_note("b");
    {
        auto transaction = doc.bus().begin_transaction("half done");
        add_note("c");
        EXPECT_FALSE(session.autosave());  // Not in the middle of a transaction.
        EXPECT_THROW(session.save(), command::CommandError);
        transaction.commit();
    }
    EXPECT_TRUE(session.autosave());
}

TEST_F(StorageSession, ModulesThatAreNotInstalledKeepTheirVersion) {
    TestDocument other;
    other.document.set_owner_enabled("gone.module", true);
    other.edit.create(model::testing::module_object("gone.module", "thing"));
    (void)save_project(dir.path() / "p.slproj", other.document, {{"gone.module", "3.1.4"}});

    session.open(dir.path() / "p.slproj");
    EXPECT_TRUE(doc.document.is_opaque(ObjectId(1)));
    add_note("mine");
    session.save();
    const std::string project = io::read_file(dir.path() / "p.slproj" / "project.json").value();
    EXPECT_NE(project.find("\"gone.module\": \"3.1.4\""), std::string::npos) << project;
}

TEST_F(StorageSession, InvalidModelNamesTheFile) {
    add_note("a");
    session.save_as(dir.path() / "p.slproj");
    const fs::path file =
        dir.path() / "p.slproj" / "model" / "ext" / "fem.demo" / "note" / "1.json";
    std::string text = io::read_file(file).value();
    text.replace(text.find("\"dependencies\": []"), 18, "\"dependencies\": [1]");
    ASSERT_TRUE(io::write_file(file, text));
    add_note("b");

    const ProjectError error = project_error([&] { session.open(dir.path() / "p.slproj"); });
    EXPECT_EQ(error.code(), ProjectErrc::invalid_model);
    EXPECT_EQ(error.file(), "model/ext/fem.demo/note/1.json");
    // Nothing changed.
    EXPECT_EQ(doc.document.size(), 2U);
    EXPECT_TRUE(session.is_modified());
}

TEST_F(StorageSession, CleanRemovesTheDataOfEarlierSaves) {
    EXPECT_EQ(project_error([&] { (void)session.clean(); }).code(), ProjectErrc::no_folder);

    GridMesh grid(30, 30, 10);
    const ObjectId mesh = doc.edit.create(model::mesh_object("mesh", grid.make()));
    session.save_as(dir.path() / "p.slproj");
    grid.move_nodes(0, 100);
    doc.edit.set_data(mesh, grid.make());
    session.save();
    grid.move_nodes(0, 100);
    doc.edit.set_data(mesh, grid.make());  // Not saved: the folder still has the second mesh.
    take_journal();
    const int notices = changes;

    const CleanStats stats = session.clean();
    EXPECT_GT(stats.removed, 0U);  // The pieces of the first mesh that the second did not keep.
    EXPECT_GT(stats.kept, 1U);
    EXPECT_EQ(take_journal(), std::vector<std::string>{"stresslab.project.clean()"});
    EXPECT_TRUE(session.is_modified());
    EXPECT_EQ(changes, notices);

    session.save();
    session.open(dir.path() / "p.slproj");
    const auto back = std::dynamic_pointer_cast<const model::Mesh>(doc.document.get(mesh).data());
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(std::equal(back->x().begin(), back->x().end(), grid.nodes.x.begin()));

    {
        auto transaction = doc.bus().begin_transaction("half done");
        EXPECT_THROW((void)session.clean(), command::CommandError);
        transaction.commit();
    }
}

}  // namespace
}  // namespace fem::storage
