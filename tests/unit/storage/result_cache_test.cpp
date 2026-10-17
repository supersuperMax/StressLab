// The cache of results (task 2.5): keys, putting and finding results, the limit of its size,
// removing the results of inactive variants, pinned results kept in the history.

#include "fem/storage/result_cache.h"

#include "fem/core/command/error.h"
#include "fem/core/io/files.h"
#include "fem/storage/error.h"
#include "fem/storage/project.h"
#include "fem/storage/repository.h"
#include "fem/storage/session.h"
#include "fem/storage/sha256.h"

#include "project_fixture.h"
#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

const SolverInfo solver{"calculix", "2.22"};

// --- Keys ----------------------------------------------------------------------------------------

ProjectFiles basic_project() {
    return read_project_files(fs::path(FEM_TEST_DATA_DIR) / "projects" / "basic.slproj");
}

TEST(StorageResultKey, IsTheSameOnEveryComputer) {
    // The golden project of task 2.1; the key is checked on every OS of the CI.
    EXPECT_EQ(result_key(basic_project(), solver),
              "befee07846e1ddad1a4d9d0088ac05c9cdfca8e087d39a397232e3e71fcd0d44");
}

TEST(StorageResultKey, DoesNotDependOnWhatDoesNotChangeAResult) {
    const ProjectFiles files = basic_project();
    const std::string key = result_key(files, solver);

    ProjectFiles other = files;
    nlohmann::json project = nlohmann::json::parse(other.at("project.json"));
    project["modules"] = {{"fem.core", "9.9.9"}, {"fem.demo", "2.0.0"}};
    project["nextObjectId"] = project["nextObjectId"].get<std::int64_t>() + 100;
    other["project.json"] = project.dump(7);  // Not even canonical.
    other[".fem/journal.slpy"] = "stresslab.cmd(\"model.create\")\n";
    EXPECT_EQ(result_key(other, solver), key);
}

TEST(StorageResultKey, ChangesWithTheModelTheMeshTheSolverAndTheInputs) {
    const ProjectFiles files = basic_project();
    const std::string key = result_key(files, solver);
    std::set<std::string> keys{key};

    ProjectFiles model_changed = files;
    const auto object =
        std::find_if(model_changed.begin(), model_changed.end(),
                     [](const auto& file) { return file.first.starts_with("model/"); });
    ASSERT_NE(object, model_changed.end());
    object->second += " ";
    keys.insert(result_key(model_changed, solver));

    ProjectFiles mesh_changed = files;
    nlohmann::json refs = nlohmann::json::parse(mesh_changed.at("refs/blobs.json"));
    ASSERT_FALSE(refs.empty());
    refs.begin().value() = "sha256:" + std::string(64, '0');
    mesh_changed["refs/blobs.json"] = refs.dump();
    keys.insert(result_key(mesh_changed, solver));

    ProjectFiles units_changed = files;
    nlohmann::json project = nlohmann::json::parse(units_changed.at("project.json"));
    project["units"]["length"] = project["units"]["length"] == "m" ? "mm" : "m";
    units_changed["project.json"] = project.dump();
    keys.insert(result_key(units_changed, solver));

    keys.insert(result_key(files, {"calculix", "2.23"}));
    keys.insert(result_key(files, {"abacus", "2.22"}));
    keys.insert(result_key(files, solver, {{"threads", "4"}}));
    keys.insert(result_key(files, solver, {{"threads", "8"}}));
    EXPECT_EQ(keys.size(), 8U);
}

TEST(StorageResultKey, RefusesFilesOfAnotherFormat) {
    ProjectFiles files = basic_project();
    files["project.json"] = R"({"format": 999})";
    EXPECT_PROJECT_ERRC((void)result_key(files, solver), ProjectErrc::unsupported_format);
}

// --- The cache -----------------------------------------------------------------------------------

class StorageResultCache : public ProjectFixture {
protected:
    StorageResultCache() : ProjectFixture("results") {
        results().set_clock([this] { return now; });
    }

    /// A result of the model now (committed first), put with these files.
    ResultInfo put_result(const std::map<std::string, std::string>& files,
                          const ResultInputs& inputs = {}) {
        const std::string version = history().snapshot("Before solving");
        const std::string key = results().key(solver, inputs);
        return results().put(key, version, solver, inputs, solver_output("out", files));
    }

    std::int64_t now = 1000;
};

TEST_F(StorageResultCache, KeyOfTheModelNowAndOfItsVersion) {
    const ObjectId skin = committed_project();
    const std::string key = results().key(solver);
    EXPECT_EQ(results().key_at("main", solver), key);
    set(skin, 3.0);
    EXPECT_NE(results().key(solver), key);
    bus().undo();
    EXPECT_EQ(results().key(solver), key);
    EXPECT_PROJECT_ERRC((void)results().key_at("nothing", solver), ProjectErrc::unknown_version);
}

TEST_F(StorageResultCache, PutAndFind) {
    (void)committed_project();
    const std::string displacement = std::string(3'000'000, 'u') + "end";
    const ResultInfo put = put_result({{utf8(u8"перемещения.frd"), displacement},
                                       {"log/solver.dat", "STEP 1\n"},
                                       {"empty.sta", ""}});
    EXPECT_EQ(put.version, *history().current_version());
    EXPECT_EQ(put.solver, solver);
    EXPECT_TRUE(put.cached);
    EXPECT_FALSE(put.pinned);
    EXPECT_EQ(put.bytes, displacement.size() + 7);
    ASSERT_EQ(put.files.size(), 3U);
    EXPECT_EQ(put.files[0].path, "empty.sta");
    EXPECT_EQ(put.files[1].path, "log/solver.dat");
    EXPECT_EQ(put.files[2].path, utf8(u8"перемещения.frd"));
    const std::string dat = "STEP 1\n";
    EXPECT_EQ(put.files[1].sha256, sha256_hex(std::as_bytes(std::span(dat))));
    EXPECT_EQ(put.created, 1000);

    now = 2000;
    const std::optional<ResultInfo> found = results().find(put.key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->used, 2000);
    EXPECT_EQ(found->created, 1000);
    EXPECT_EQ(found->files, put.files);
    ASSERT_TRUE(found->folder.has_value());
    EXPECT_EQ(read_bytes(*found->folder / fs::path(u8"перемещения.frd")), displacement);
    EXPECT_EQ(read_bytes(*found->folder / "log" / "solver.dat"), "STEP 1\n");
    EXPECT_TRUE(fs::is_regular_file(*found->folder / "empty.sta"));
    // In the cache folder of the project, under its key.
    EXPECT_EQ(found->folder->parent_path(),
              folder() / fs::path(u8".fem/cache/results") / fs::path(put.key));

    EXPECT_FALSE(results().find(std::string(64, 'a')).has_value());
    EXPECT_FALSE(results().find("not a key").has_value());
    EXPECT_EQ(results().usage(), put.bytes);
    ASSERT_EQ(results().list().size(), 1U);
    EXPECT_EQ(results().list().front().used, 2000);
}

TEST_F(StorageResultCache, AResultBelongsToTheModelOfItsVersion) {
    const ObjectId skin = committed_project();
    const std::string version = *history().current_version();
    const std::string key = results().key(solver);
    const fs::path out = solver_output("out", {{"a.frd", "1"}});
    set(skin, 3.0);
    // The key of the model now is not that of the version.
    EXPECT_PROJECT_ERRC((void)results().put(results().key(solver), version, solver, {}, out),
                        ProjectErrc::result_mismatch);
    EXPECT_PROJECT_ERRC((void)results().put(key, version, {"calculix", "2.23"}, {}, out),
                        ProjectErrc::result_mismatch);
    EXPECT_PROJECT_ERRC((void)results().put(key, version, solver, {{"threads", "4"}}, out),
                        ProjectErrc::result_mismatch);
    EXPECT_PROJECT_ERRC((void)results().put(key, "nothing", solver, {}, out),
                        ProjectErrc::unknown_version);
    EXPECT_PROJECT_ERRC((void)results().put(key, version, solver, {}, dir.path() / "missing"),
                        ProjectErrc::io);
    EXPECT_TRUE(results().list().empty());
    // The version it was computed from takes it, whatever the model is now.
    EXPECT_EQ(results().put(key, version, solver, {}, out).version, version);
}

TEST_F(StorageResultCache, NeedsAFolderAndAVersion) {
    (void)add("section", "skin");
    EXPECT_PROJECT_ERRC((void)results().put(std::string(64, 'a'), "main", solver, {}, dir.path()),
                        ProjectErrc::no_folder);
    EXPECT_FALSE(results().find(std::string(64, 'a')).has_value());
    EXPECT_TRUE(results().list().empty());
    EXPECT_PROJECT_ERRC(results().pin(std::string(64, 'a')), ProjectErrc::no_folder);
    EXPECT_PROJECT_ERRC((void)results().prune_inactive(), ProjectErrc::no_folder);
    session.save_as(folder());
    EXPECT_PROJECT_ERRC((void)results().put(results().key(solver), "main", solver, {}, dir.path()),
                        ProjectErrc::unknown_version);
}

TEST_F(StorageResultCache, PutReplacesTheResultOfTheSameKey) {
    (void)committed_project();
    const ResultInfo first = put_result({{"a.frd", "first"}, {"old.dat", "x"}});
    const ResultInfo second = put_result({{"a.frd", "second"}});
    EXPECT_EQ(second.key, first.key);
    ASSERT_EQ(results().list().size(), 1U);
    EXPECT_EQ(files_below(*second.folder),
              (std::map<std::string, std::string>{{"a.frd", "second"}}));
}

TEST_F(StorageResultCache, LeftoversOfACrashGoAtTheNextPut) {
    (void)committed_project();
    const fs::path cache = folder() / fs::path(u8".fem/cache/results");
    write_bytes(cache / ".put-123" / "files" / "a.frd", "half");
    write_bytes(cache / std::string(64, 'b') / "files" / "a.frd", "no entry.json");
    (void)put_result({{"a.frd", "1"}});
    EXPECT_FALSE(fs::exists(cache / ".put-123"));
    EXPECT_FALSE(fs::exists(cache / std::string(64, 'b')));
    EXPECT_EQ(results().list().size(), 1U);
}

TEST_F(StorageResultCache, TheLimitRemovesTheResultsUsedLongestAgo) {
    const ObjectId skin = committed_project();
    const std::string data(1000, 'r');
    std::vector<std::string> keys;
    for (int i = 0; i < 3; ++i) {
        set(skin, 3.0 + i);
        now = 1000 + i;
        keys.push_back(put_result({{"a.frd", data}}).key);
    }
    results().set_limit(3500);
    now = 2000;
    ASSERT_TRUE(results().find(keys[0]).has_value());  // Now the one used last.

    set(skin, 10.0);
    now = 3000;
    keys.push_back(put_result({{"a.frd", data}}).key);
    std::vector<std::string> left;
    for (const ResultInfo& info : results().list()) {
        left.push_back(info.key);
    }
    EXPECT_EQ(left, (std::vector<std::string>{keys[3], keys[0], keys[2]}));
    EXPECT_EQ(results().usage(), 3000U);

    // A result larger than the limit stays alone.
    results().set_limit(10);
    set(skin, 11.0);
    keys.push_back(put_result({{"a.frd", data}}).key);
    ASSERT_EQ(results().list().size(), 1U);
    EXPECT_EQ(results().list().front().key, keys[4]);

    // No limit.
    results().set_limit(0);
    set(skin, 12.0);
    (void)put_result({{"a.frd", data}});
    EXPECT_EQ(results().list().size(), 2U);
}

TEST_F(StorageResultCache, PruneRemovesTheResultsOfInactiveVariants) {
    const ObjectId skin = committed_project();
    const ResultInfo base = put_result({{"a.frd", "base"}});
    const std::string variant = utf8(u8"Композитная обшивка");
    history().branch(variant);
    history().checkout(variant);
    set(skin, 3.0);
    const ResultInfo composite = put_result({{"a.frd", "composite"}});
    history().checkout("main");
    set(skin, 4.0);
    const ResultInfo later = put_result({{"a.frd", "later"}});
    // A result nobody can reach: its version was put aside and forgotten.
    set(skin, 5.0);
    history().stash();
    const std::string stashed = history().stashes().front().id;
    const std::string stashed_key = results().key_at(stashed, solver);
    (void)results().put(stashed_key, stashed, solver, {},
                        solver_output("stashed", {{"a.frd", "stashed"}}));
    history().stash_drop();
    take_journal();

    // On the variant: its versions are the version it was made at and its own.
    history().checkout(variant);
    PruneStats stats = results().prune_inactive();
    EXPECT_EQ(stats.removed, 2U);
    EXPECT_EQ(stats.freed_bytes, 5U + 7U);
    std::set<std::string> left;
    for (const ResultInfo& info : results().list()) {
        left.insert(info.key);
    }
    EXPECT_EQ(left, (std::set<std::string>{base.key, composite.key}));

    history().checkout("main");
    stats = results().prune_inactive();
    EXPECT_EQ(stats.removed, 1U);
    ASSERT_EQ(results().list().size(), 1U);
    EXPECT_EQ(results().list().front().key, base.key);
    (void)later;
    EXPECT_EQ(
        take_journal(),
        (std::vector<std::string>{
            "stresslab.history.checkout(\"" + variant + "\")", "stresslab.results.prune_inactive()",
            "stresslab.history.checkout(\"main\")", "stresslab.results.prune_inactive()"}));
}

TEST_F(StorageResultCache, APinnedResultIsKeptInTheHistory) {
    (void)committed_project();
    const std::string frd = std::string(2'000'000, 'p') + "!";
    const ResultInfo put = put_result({{utf8(u8"напряжения.frd"), frd}, {"b.dat", "dat"}});
    take_journal();
    results().pin(put.key);
    EXPECT_EQ(take_journal(),
              (std::vector<std::string>{"stresslab.results.pin(\"" + put.key + "\")"}));
    ASSERT_TRUE(results().find(put.key)->pinned);

    // In the history: a record under refs/results, the files in the blob store.
    const std::optional<Repository> repo = Repository::open(folder());
    ASSERT_TRUE(repo);
    const std::map<std::string, std::string> pins = repo->pins();
    ASSERT_EQ(pins.size(), 1U);
    EXPECT_EQ(pins.begin()->first, put.key);
    const nlohmann::json record = nlohmann::json::parse(pins.begin()->second);
    EXPECT_EQ(record.at("version"), put.version);
    EXPECT_TRUE(
        is_blob_ref(record.at("files").at(utf8(u8"напряжения.frd")).at("data").get<std::string>()));

    // Removed from the cache, it comes back from the history byte for byte, even after a clean.
    EXPECT_TRUE(results().remove(put.key));
    EXPECT_EQ(results().usage(), 0U);
    ASSERT_EQ(results().list().size(), 1U);
    EXPECT_FALSE(results().list().front().cached);
    EXPECT_TRUE(results().list().front().pinned);
    const CleanStats cleaned = session.clean();
    (void)cleaned;
    now = 5000;
    const std::optional<ResultInfo> back = results().find(put.key);
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->cached);
    EXPECT_EQ(back->created, put.created);
    EXPECT_EQ(back->used, 5000);
    EXPECT_EQ(back->files, put.files);
    EXPECT_EQ(read_bytes(*back->folder / fs::path(u8"напряжения.frd")), frd);

    // Pinning again does nothing; unpinned, its blocks go with the next clean.
    results().pin(put.key);
    take_journal();
    results().unpin(put.key);
    EXPECT_EQ(take_journal(),
              (std::vector<std::string>{"stresslab.results.unpin(\"" + put.key + "\")"}));
    EXPECT_FALSE(results().find(put.key)->pinned);
    EXPECT_GT(session.clean().removed, 0U);
    EXPECT_TRUE(results().remove(put.key));
    EXPECT_FALSE(results().find(put.key).has_value());
    EXPECT_PROJECT_ERRC(results().unpin(put.key), ProjectErrc::unknown_result);
}

TEST_F(StorageResultCache, APinnedResultOutlivesTheSessionAndItsVersion) {
    const ObjectId mesh = doc.edit.create(model::mesh_object("wing", GridMesh(40, 30, 10).make()));
    session.save_as(folder());
    (void)history().commit("Mesh");
    doc.edit.set_data(mesh, GridMesh(20, 10, 5).make());
    // Computed from changes put aside and then forgotten: no variant reaches the version.
    history().stash();
    const std::string stashed = history().stashes().front().id;
    const std::string key = results().key_at(stashed, solver);
    (void)results().put(key, stashed, solver, {}, solver_output("out", {{"a.frd", "a"}}));
    results().pin(key);
    history().stash_drop();
    (void)results().remove(key);
    (void)session.clean();

    TestDocument other;
    ProjectSession reopened(other.bus());
    reopened.open(folder());
    const std::optional<ResultInfo> found = reopened.results().find(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(read_bytes(*found->folder / "a.frd"), "a");
    // The mesh of its version is still there.
    const std::optional<Repository> repo = Repository::open(folder());
    const FolderBlobStore blobs(folder() / fs::path(u8".fem/objects"));
    const ProjectFiles files = repo->read(stashed).files;
    for (const std::string& root : read_blob_roots(files)) {
        EXPECT_NO_THROW((void)get_object_data(root, blobs));
    }
}

TEST_F(StorageResultCache, PinRefusals) {
    (void)committed_project();
    EXPECT_PROJECT_ERRC(results().pin(std::string(64, 'a')), ProjectErrc::unknown_result);
    EXPECT_PROJECT_ERRC(results().pin("not a key"), ProjectErrc::unknown_result);
    const ResultInfo put = put_result({{"a.frd", std::string(100'000, 'a')}});

    int asked = 0;
    const Progress cancelling{.report = {}, .cancelled = [&asked] { return ++asked > 1; }};
    EXPECT_PROJECT_ERRC(results().pin(put.key, cancelling), ProjectErrc::cancelled);
    EXPECT_FALSE(results().find(put.key)->pinned);

    {
        command::Transaction transaction = bus().begin_transaction("pinning inside");
        try {
            results().pin(put.key);
            ADD_FAILURE() << "pinned inside a transaction";
        } catch (const command::CommandError& error) {
            EXPECT_EQ(error.code(), command::CommandErrc::invalid_state);
        }
        transaction.rollback();
    }

    std::vector<double> reported;
    results().pin(put.key, {.report = [&reported](double done) { reported.push_back(done); }});
    ASSERT_FALSE(reported.empty());
    EXPECT_TRUE(std::is_sorted(reported.begin(), reported.end()));
    EXPECT_DOUBLE_EQ(reported.back(), 1.0);
}

TEST_F(StorageResultCache, RemoveIsJournaled) {
    (void)committed_project();
    const ResultInfo put = put_result({{"a.frd", "a"}});
    take_journal();
    EXPECT_TRUE(results().remove(put.key));
    EXPECT_FALSE(results().remove(put.key));
    EXPECT_EQ(take_journal(),
              (std::vector<std::string>{"stresslab.results.remove(\"" + put.key + "\")",
                                        "stresslab.results.remove(\"" + put.key + "\")"}));
}

}  // namespace
}  // namespace fem::storage
