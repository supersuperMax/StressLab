#include "fem/storage/garbage.h"

#include "fem/core/io/files.h"
#include "fem/core/model/object.h"
#include "fem/storage/error.h"
#include "fem/storage/project.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

/// The blocks of a folder store.
std::set<std::string> blocks_of(const FolderBlobStore& store) {
    std::set<std::string> result;
    for (const FolderBlobStore::File& file : store.files()) {
        result.insert(file.ref);
    }
    return result;
}

/// The table of `ref` and its pieces.
std::set<std::string> reachable_from(const FolderBlobStore& store, const std::string& ref) {
    std::set<std::string> result{ref};
    for (const std::string& piece : object_data_pieces(store.get(ref))) {
        result.insert(piece);
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

TEST(StorageGarbage, KeepsWhatTheRootsReachAndRemovesTheRest) {
    TempDir dir("garbage");
    FolderBlobStore store(dir.path() / "objects");
    GridMesh grid(30, 30, 10);
    const std::string old_mesh = put_object_data(*grid.make(), store);
    grid.move_nodes(0, 200);
    const std::string mesh = put_object_data(*grid.make(), store);
    const std::string set = put_object_data(model::NodeSet({1, 2, 3}), store);
    const std::string extra = store.put(Bytes(100, std::byte{7}));  // Not a table: no root.
    fs::path tmp = store.file_of(mesh);
    tmp += ".tmp";
    ASSERT_TRUE(io::write_file(tmp, "left by a crash"));

    std::set<std::string> expected = reachable_from(store, mesh);
    expected.insert(set);
    const std::set<std::string> before = blocks_of(store);
    ASSERT_TRUE(before.contains(old_mesh));

    const std::vector<std::string> roots = {mesh, set, mesh};  // Repeats are harmless.
    const CleanStats stats = collect_garbage(store, roots);
    std::set<std::string> after = blocks_of(store);
    EXPECT_EQ(after, expected);
    EXPECT_FALSE(after.contains(old_mesh));
    EXPECT_FALSE(after.contains(extra));
    EXPECT_FALSE(fs::exists(tmp));
    EXPECT_EQ(stats.kept, expected.size());
    EXPECT_EQ(stats.removed, before.size() - expected.size());  // The temporary file is "".
    EXPECT_GT(stats.freed_bytes, 0U);
    // Pieces both meshes share are kept: the data reads back.
    EXPECT_EQ(describe_object_data(*get_object_data(mesh, store)),
              describe_object_data(*grid.make()));

    // Again: nothing more to do.
    const CleanStats again = collect_garbage(store, roots);
    EXPECT_EQ(again.removed, 0U);
    EXPECT_EQ(again.kept, stats.kept);
    EXPECT_EQ(again.kept_bytes, stats.kept_bytes);

    // No roots: everything goes, and the empty folders with it.
    (void)collect_garbage(store, {});
    EXPECT_TRUE(store.files().empty());
    EXPECT_TRUE(fs::is_empty(dir.path() / "objects"));
}

TEST(StorageGarbage, RemovesNothingIfARootCannotBeRead) {
    TempDir dir("garbage-damaged");
    FolderBlobStore store(dir.path() / "objects");
    const std::string mesh = put_object_data(*GridMesh(20, 20, 5).make(), store);
    const std::string loose = store.put(Bytes(10, std::byte{1}));
    const std::set<std::string> before = blocks_of(store);

    // A missing root.
    const std::string missing = blob_ref(Bytes(3, std::byte{2}));
    std::vector<std::string> roots = {mesh, missing};
    EXPECT_EQ(error_code([&] { (void)collect_garbage(store, roots); }), ProjectErrc::missing_blob);
    EXPECT_EQ(blocks_of(store), before);
    // A root that is not a table of contents.
    roots = {mesh, loose};
    EXPECT_EQ(error_code([&] { (void)collect_garbage(store, roots); }), ProjectErrc::corrupt_blob);
    EXPECT_EQ(blocks_of(store), before);
    // A damaged root.
    ASSERT_TRUE(io::write_file(store.file_of(mesh), "garbage"));
    roots = {mesh};
    EXPECT_EQ(error_code([&] { (void)collect_garbage(store, roots); }), ProjectErrc::corrupt_blob);
    EXPECT_EQ(blocks_of(store), before);
}

TEST(StorageGarbage, CleanProjectKeepsTheSavedProject) {
    TempDir dir("garbage-project");
    const fs::path folder = dir.path() / fs::path(u8"крыло.slproj");
    TestDocument doc;
    GridMesh grid(40, 40, 20);
    const ObjectId mesh = doc.edit.create(model::mesh_object("mesh", grid.make()));
    doc.edit.create(model::node_set_object(
        mesh, "fixed", std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 1})));
    (void)save_project(folder, doc.document, {});
    for (int version = 0; version < 3; ++version) {
        grid.move_nodes(static_cast<std::size_t>(version) * 5000, 300);
        doc.edit.set_data(mesh, grid.make());
        (void)save_project(folder, doc.document, {});
    }
    const std::string saved = describe(doc.document);
    const FolderBlobStore store(folder / ".fem" / "objects");
    const std::size_t blocks = blocks_of(store).size();

    const CleanStats stats = clean_project(folder);
    EXPECT_GT(stats.removed, 0U);
    EXPECT_EQ(stats.kept + stats.removed, blocks);

    // Every reachable block is still there: the project opens with the same model and saving it
    // again writes nothing.
    TestDocument loaded;
    loaded.bus().replace_document(load_project(folder).contents);
    EXPECT_EQ(describe(loaded.document), saved);
    const SaveStats again = save_project(folder, loaded.document, {});
    EXPECT_EQ(again.written, 0U);
    EXPECT_EQ(blocks_of(store).size(), stats.kept);

    // Roots from elsewhere (the history of task 2.3) keep their blocks.
    GridMesh other(10, 10, 10);
    FolderBlobStore writable(folder / ".fem" / "objects");
    const std::string kept = put_object_data(*other.make(), writable);
    const std::vector<std::string> extra = {kept};
    (void)clean_project(folder, extra);
    EXPECT_TRUE(store.contains(kept));
    (void)clean_project(folder);
    EXPECT_FALSE(store.contains(kept));
}

TEST(StorageGarbage, CleanProjectRefusesWhatItCannotRead) {
    TempDir dir("garbage-refuse");
    EXPECT_EQ(error_code([&] { (void)clean_project(dir.path()); }), ProjectErrc::not_a_project);

    const fs::path folder = dir.path() / "p.slproj";
    TestDocument doc;
    doc.edit.create(model::mesh_object("mesh", model::testing::small_mesh()));
    (void)save_project(folder, doc.document, {});
    const FolderBlobStore store(folder / ".fem" / "objects");
    const std::set<std::string> before = blocks_of(store);

    const fs::path refs = folder / "refs" / "blobs.json";
    const std::string text = io::read_file(refs).value();
    fs::remove(refs);
    EXPECT_EQ(error_code([&] { (void)clean_project(folder); }), ProjectErrc::io);
    ASSERT_TRUE(io::write_file(refs, "{oops"));
    EXPECT_EQ(error_code([&] { (void)clean_project(folder); }), ProjectErrc::invalid_file);
    ASSERT_TRUE(io::write_file(refs, text));

    const fs::path project = folder / "project.json";
    std::string header = io::read_file(project).value();
    header.replace(header.find("\"format\": 2"), 11, "\"format\": 3");
    ASSERT_TRUE(io::write_file(project, header));
    EXPECT_EQ(error_code([&] { (void)clean_project(folder); }), ProjectErrc::unsupported_format);
    EXPECT_EQ(blocks_of(store), before);
}

}  // namespace
}  // namespace fem::storage
