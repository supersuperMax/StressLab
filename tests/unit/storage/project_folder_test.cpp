#include "fem/core/io/files.h"
#include "fem/core/model/object.h"
#include "fem/storage/error.h"
#include "fem/storage/project.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <map>
#include <string>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

constexpr const char* module = "fem.demo";

/// Every file below `folder` by its path with '/'.
std::map<std::string, std::string> folder_files(const fs::path& folder) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            result.emplace(io::display(entry.path().lexically_relative(folder)),
                           io::read_file(entry.path()).value());
        }
    }
    return result;
}

ProjectErrc error_code(const std::function<void()>& action) {
    try {
        action();
    } catch (const ProjectError& error) {
        return error.code();
    }
    ADD_FAILURE() << "no ProjectError";
    return ProjectErrc::io;
}

/// The model of tests/data/projects/basic.slproj: a mesh with its sets, objects of a module with
/// a dependency and text that needs escaping, and a removed object (a gap in the ids).
void build_basic(TestDocument& doc) {
    doc.document.set_owner_enabled(module, true);
    doc.edit.set_units(model::UnitSystem::mm_t_s());
    const ObjectId mesh =
        doc.edit.create(model::mesh_object("Кронштейн", model::testing::small_mesh()));
    const ObjectId fixed = doc.edit.create(model::node_set_object(
        mesh, "Заделка",
        std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 1, 2})));
    doc.edit.create(model::element_set_object(
        mesh, "tets", std::make_shared<const model::ElementSet>(std::vector<model::Index>{0, 1})));
    doc.edit.create(model::surface_object(
        mesh, "top",
        std::make_shared<const model::Surface>(std::vector<model::ElementSide>{{0, 2}})));
    const ObjectId gone = doc.edit.create(model::testing::module_object(module, "note", "draft"));
    model::NewObject note =
        model::testing::module_object(module, "note", "Опора \"A\"", {}, {fixed});
    note.properties = {{"E", 210000.0},
                       {"nu", 0.3},
                       {"count", 3},
                       {"text", "строка\nвторая"},
                       {"list", {1, 2.5, nullptr, true}}};
    doc.edit.create(std::move(note));
    (void)doc.edit.remove(gone);
}

fs::path golden_folder() {
    return fs::path(FEM_TEST_DATA_DIR) / "projects" / "basic.slproj";
}

const ModuleVersions basic_versions = {{"fem.core", "0.1.0"}, {module, "0.1.0"}};

TEST(StorageProjectFolder, GoldenProjectHasTheExpectedBytes) {
    TestDocument doc;
    build_basic(doc);
    if (environment("STRESSLAB_UPDATE_GOLDEN")) {
        fs::remove_all(golden_folder());
        (void)save_project(golden_folder(), doc.document, basic_versions);
    }
    TempDir dir("golden");
    const fs::path folder = dir.path() / "basic.slproj";
    (void)save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(folder_files(folder), folder_files(golden_folder()))
        << "the format changed: if on purpose, run once with STRESSLAB_UPDATE_GOLDEN=1 and review";
}

TEST(StorageProjectFolder, GoldenProjectLoadsAndSavesToTheSameBytes) {
    TestDocument doc;
    doc.bus().replace_document(load_project(golden_folder()).contents);
    TempDir dir("golden-again");
    const fs::path folder = dir.path() / "copy.slproj";
    (void)save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(folder_files(folder), folder_files(golden_folder()));

    TestDocument built;
    build_basic(built);
    EXPECT_EQ(describe(doc.document), describe(built.document));
}

TEST(StorageProjectFolder, SavesOnlyWhatChanged) {
    TempDir dir("only-changes");
    const fs::path folder = dir.path() / fs::path(u8"проект.slproj");
    TestDocument doc;
    build_basic(doc);

    const SaveStats first = save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(first.written, 7U);  // project, blobs refs and five objects
    EXPECT_TRUE(fs::is_directory(folder / ".fem" / "objects" / "sha256"));

    const SaveStats unchanged = save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(unchanged.written, 0U);
    EXPECT_EQ(unchanged.removed, 0U);

    const ObjectId note = doc.document.find_by_type("note").front();
    doc.edit.set_properties(note, {{"E", 70000.0}});
    const SaveStats one = save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(one.written, 1U);
    EXPECT_EQ(one.removed, 0U);
}

TEST(StorageProjectFolder, RemovesFilesOfRemovedObjects) {
    TempDir dir("removed");
    const fs::path folder = dir.path() / "p.slproj";
    TestDocument doc;
    build_basic(doc);
    (void)save_project(folder, doc.document, basic_versions);
    ASSERT_TRUE(fs::exists(folder / "model" / "ext" / "fem.demo" / "note"));

    // A temporary file left by a crash goes too.
    ASSERT_TRUE(io::write_file(folder / "model" / "mesh" / "1.json.tmp", "half"));
    (void)doc.edit.remove(doc.document.find_by_type("note").front());
    const SaveStats stats = save_project(folder, doc.document, basic_versions);
    EXPECT_EQ(stats.removed, 2U);
    EXPECT_FALSE(fs::exists(folder / "model" / "ext"));  // Empty folders are removed.
    EXPECT_FALSE(fs::exists(folder / "model" / "mesh" / "1.json.tmp"));

    TestDocument loaded;
    loaded.bus().replace_document(load_project(folder).contents);
    EXPECT_EQ(describe(loaded.document), describe(doc.document));
}

TEST(StorageProjectFolder, TypesThatDifferOnlyInCase) {
    // Where the file system ignores case, "Note/" is the folder "note/" of the earlier save.
    TempDir dir("case");
    const fs::path folder = dir.path() / "p.slproj";
    TestDocument doc;
    doc.document.set_owner_enabled(module, true);
    const ObjectId lower = doc.edit.create(model::testing::module_object(module, "note"));
    (void)save_project(folder, doc.document, {});
    doc.edit.create(model::testing::module_object(module, "Note"));
    (void)save_project(folder, doc.document, {});
    (void)doc.edit.remove(lower);
    (void)save_project(folder, doc.document, {});

    TestDocument loaded;
    loaded.bus().replace_document(load_project(folder).contents);
    EXPECT_EQ(describe(loaded.document), describe(doc.document));
    EXPECT_EQ(save_project(folder, loaded.document, {}).written, 0U);
}

TEST(StorageProjectFolder, DoesNotFillOtherFolders) {
    TempDir dir("other");
    TestDocument doc;
    ASSERT_TRUE(io::write_file(dir.path() / "notes.txt", "mine"));
    EXPECT_EQ(error_code([&] { (void)save_project(dir.path(), doc.document, {}); }),
              ProjectErrc::not_a_project);
    EXPECT_EQ(error_code([&] { (void)save_project(dir.path() / "notes.txt", doc.document, {}); }),
              ProjectErrc::not_a_project);
    EXPECT_EQ(io::read_file(dir.path() / "notes.txt"), "mine");

    // An empty folder is fine.
    fs::create_directories(dir.path() / "empty");
    (void)save_project(dir.path() / "empty", doc.document, {});
    EXPECT_TRUE(is_project_folder(dir.path() / "empty"));
}

TEST(StorageProjectFolder, LoadingSomethingElse) {
    TempDir dir("not-a-project");
    EXPECT_EQ(error_code([&] { (void)load_project(dir.path()); }), ProjectErrc::not_a_project);
    EXPECT_EQ(error_code([&] { (void)load_project(dir.path() / "missing"); }),
              ProjectErrc::not_a_project);
}

TEST(StorageProjectFolder, DamagedBlockOnDisk) {
    TempDir dir("damaged");
    const fs::path folder = dir.path() / "p.slproj";
    TestDocument doc;
    build_basic(doc);
    (void)save_project(folder, doc.document, basic_versions);
    for (const auto& entry : fs::recursive_directory_iterator(folder / ".fem" / "objects")) {
        if (entry.is_regular_file()) {
            ASSERT_TRUE(io::write_file(entry.path(), "garbage"));
        }
    }
    EXPECT_EQ(error_code([&] { (void)load_project(folder); }), ProjectErrc::corrupt_blob);
}

TEST(StorageProjectFolder, CheckedOutWithWindowsLineBreaks) {
    // git with core.autocrlf may turn \n into \r\n in a checkout: the project still opens, and
    // saving writes canonical files again.
    TempDir dir("crlf");
    const fs::path folder = dir.path() / "p.slproj";
    TestDocument doc;
    build_basic(doc);
    (void)save_project(folder, doc.document, basic_versions);
    const fs::path file = folder / "project.json";
    std::string text = io::read_file(file).value();
    std::string crlf;
    for (const char c : text) {
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    }
    ASSERT_TRUE(io::write_file(file, crlf));

    TestDocument loaded;
    loaded.bus().replace_document(load_project(folder).contents);
    EXPECT_EQ(save_project(folder, loaded.document, basic_versions).written, 1U);
    EXPECT_EQ(io::read_file(file), text);
}

}  // namespace
}  // namespace fem::storage
