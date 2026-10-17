// The git repository of a project folder (task 2.3): versions of `ProjectFiles` made and read
// back byte for byte, variants and labels with the names users give them, stashes and the walk of
// the history.

#include "fem/storage/repository.h"

#include "fem/core/io/files.h"
#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;

const Author author{"Инженер", "engineer@example.com"};

std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// `count` four-byte characters: few enough, but too long as a file name.
std::string emoji(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += "ð";
    }
    return text;
}

ProjectFiles files_with(const std::string& thickness) {
    return {
        {"project.json", "{\n  \"format\": 2\n}\n"},
        {"model/ext/fem.demo/note/3.json", "{\n  \"thickness\": " + thickness + "\n}\n"},
        {"model/ext/fem.demo/note/4.json", "{}\n"},
        {"refs/blobs.json", "{}\n"},
    };
}

VersionNote note(const std::string& message, VersionKind kind = VersionKind::manual) {
    return {.message = message, .kind = kind};
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

class StorageRepository : public ::testing::Test {
protected:
    StorageRepository() : dir("repository"), repo(Repository::open_or_create(dir.path())) {}

    TempDir dir;
    Repository repo;
};

}  // namespace

TEST(StorageRepositoryOpen, ProjectWithoutHistoryHasNone) {
    TempDir dir("no history");
    EXPECT_FALSE(Repository::open(dir.path()).has_value());
    (void)Repository::open_or_create(dir.path());
    EXPECT_TRUE(fs::is_directory(dir.path() / ".fem" / "git"));
    EXPECT_TRUE(Repository::open(dir.path()).has_value());
}

TEST_F(StorageRepository, NewHistoryIsOnMainWithoutVersions) {
    const Head head = repo.head();
    EXPECT_EQ(head.branch, std::optional<std::string>("main"));
    EXPECT_FALSE(head.version.has_value());
    EXPECT_TRUE(repo.branches().empty());
    EXPECT_TRUE(repo.reachable().empty());
}

TEST_F(StorageRepository, VersionHoldsTheFilesAndTheJournalByteForByte) {
    ProjectFiles files = files_with("2.0");
    files.emplace("model/ext/fem.demo/note/5.json", utf8(u8"{\n  \"name\": \"Обшивка\"\r\n}"));
    const std::string journal = "# StressLab session journal\nstresslab.undo()\n";

    const std::string id = repo.commit(files, journal, note("First"), author, {});
    const VersionFiles read = repo.read(id);

    EXPECT_EQ(read.files, files);
    EXPECT_EQ(read.journal, journal);
    EXPECT_EQ(id.size(), 40U);
}

TEST_F(StorageRepository, ReopenedHistoryReadsTheSame) {
    const std::string id = repo.commit(files_with("2.0"), "", note("First"), author, {});
    repo.set_branch("main", id, false);

    std::optional<Repository> again = Repository::open(dir.path());
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->head().version, std::optional<std::string>(id));
    EXPECT_EQ(again->read(id).files, files_with("2.0"));
    EXPECT_TRUE(again->read(id).journal.empty());
}

TEST_F(StorageRepository, InfoGivesTheMessageAuthorAndKind) {
    const std::string first =
        repo.commit(files_with("2.0"), "", note("  Two ribs\n\nDetails\n"), author, {});
    VersionNote automatic = note("Before fem.mesh.rebuild", VersionKind::automatic);
    automatic.before = "fem.mesh.rebuild";
    const std::string second =
        repo.commit(files_with("3.0"), "", automatic, author, std::vector<std::string>{first});

    const VersionInfo one = repo.info(first);
    EXPECT_EQ(one.message, "Two ribs\n\nDetails");
    EXPECT_EQ(one.summary, "Two ribs");
    EXPECT_EQ(one.kind, VersionKind::manual);
    EXPECT_EQ(one.author.name, author.name);
    EXPECT_EQ(one.author.email, author.email);
    EXPECT_GT(one.time, 1'600'000'000);
    EXPECT_TRUE(one.parents.empty());

    const VersionInfo two = repo.info(second);
    EXPECT_EQ(two.message, "Before fem.mesh.rebuild");
    EXPECT_EQ(two.kind, VersionKind::automatic);
    EXPECT_EQ(two.before, "fem.mesh.rebuild");
    EXPECT_EQ(two.parents, std::vector<std::string>{first});
}

TEST_F(StorageRepository, RestoreAndStashNotesAreReadBack) {
    const std::string first = repo.commit(files_with("2.0"), "", note("First"), author, {});
    VersionNote restore = note("Restored", VersionKind::restore);
    restore.restored = first;
    VersionNote stash = note(utf8(u8"Отложено"), VersionKind::stash);
    stash.branch = utf8(u8"Композитная обшивка");

    const VersionInfo restored = repo.info(
        repo.commit(files_with("2.0"), "", restore, author, std::vector<std::string>{first}));
    const VersionInfo stashed = repo.info(
        repo.commit(files_with("4.0"), "", stash, author, std::vector<std::string>{first}));

    EXPECT_EQ(restored.kind, VersionKind::restore);
    EXPECT_EQ(restored.restored, first);
    EXPECT_EQ(restored.message, "Restored");
    EXPECT_EQ(stashed.kind, VersionKind::stash);
    EXPECT_EQ(stashed.branch, utf8(u8"Композитная обшивка"));
    EXPECT_EQ(stashed.message, utf8(u8"Отложено"));
}

TEST_F(StorageRepository, VariantsAndLabelsKeepTheNamesOfUsers) {
    const std::string id = repo.commit(files_with("2.0"), "", note("First"), author, {});
    const std::vector<std::string> names = {
        utf8(u8"Композитная обшивка"),
        "Sent 12.10",
        "a/b",
        "..",
        "x.lock",
        "~^:?*[\\",
        "%41",
        "aux",
        "@{0}",
        "-",
        ".hidden",
    };
    for (const std::string& name : names) {
        repo.set_branch(name, id, true);
        repo.create_tag(name, id);
    }
    for (const std::string& name : names) {
        EXPECT_EQ(repo.branches().at(name), id) << name;
        EXPECT_EQ(repo.tags().at(name), id) << name;
        EXPECT_EQ(repo.resolve(name), id) << name;
    }
    EXPECT_EQ(repo.branches().size(), names.size());
}

TEST_F(StorageRepository, LongCyrillicNamesFitIntoPathsOfWindows) {
    const std::string id = repo.commit(files_with("2.0"), "", note("First"), author, {});
    const std::string name = utf8(u8"Отправлено на согласование в отдел прочности 12.10");
    repo.create_tag(name, id);
    EXPECT_EQ(repo.resolve(name), id);
}

TEST_F(StorageRepository, NamesThatCannotNameAVariantAreRefused) {
    const std::string id = repo.commit(files_with("2.0"), "", note("First"), author, {});
    for (const std::string& name :
         {std::string(), std::string("two\nlines"), std::string("tab\there"),
          std::string("\xC0\xAF"), std::string(65, 'a'), emoji(51)}) {
        EXPECT_PROJECT_ERROR(repo.set_branch(name, id, true), ProjectErrc::invalid_name);
        EXPECT_PROJECT_ERROR(repo.create_tag(name, id), ProjectErrc::invalid_name);
    }
}

TEST_F(StorageRepository, ExistingNamesAreRefusedWhateverTheCase) {
    const std::string id = repo.commit(files_with("2.0"), "", note("First"), author, {});
    repo.set_branch("Variant", id, true);
    repo.create_tag("Label", id);

    EXPECT_PROJECT_ERROR(repo.set_branch("Variant", id, true), ProjectErrc::already_exists);
    EXPECT_PROJECT_ERROR(repo.set_branch("variant", id, true), ProjectErrc::already_exists);
    EXPECT_PROJECT_ERROR(repo.create_tag("LABEL", id), ProjectErrc::already_exists);
    // Moving an existing variant is what a commit does.
    repo.set_branch("Variant", id, false);
    // Cyrillic is kept as it is in references, and its case is ignored as well.
    repo.set_branch(utf8(u8"Вариант Ё"), id, true);
    EXPECT_PROJECT_ERROR(repo.set_branch(utf8(u8"вариант ё"), id, true),
                         ProjectErrc::already_exists);
    repo.set_branch(utf8(u8"Вариант Ж"), id, true);
}

TEST_F(StorageRepository, ResolveFindsVariantsLabelsIdsAndStepsBack) {
    const std::string first = repo.commit(files_with("1.0"), "", note("1"), author, {});
    const std::string second =
        repo.commit(files_with("2.0"), "", note("2"), author, std::vector<std::string>{first});
    const std::string third =
        repo.commit(files_with("3.0"), "", note("3"), author, std::vector<std::string>{second});
    repo.set_branch("main", third, false);
    repo.create_tag("approved", second);

    EXPECT_EQ(repo.resolve("main"), third);
    EXPECT_EQ(repo.resolve("approved"), second);
    EXPECT_EQ(repo.resolve(first), first);
    EXPECT_EQ(repo.resolve(first.substr(0, 12)), first);
    EXPECT_EQ(repo.resolve("main~2"), first);
    EXPECT_EQ(repo.resolve("approved~1"), first);
    EXPECT_EQ(repo.resolve(third + "~0"), third);
    EXPECT_PROJECT_ERROR((void)repo.resolve("main~3"), ProjectErrc::unknown_version);
    EXPECT_PROJECT_ERROR((void)repo.resolve("other"), ProjectErrc::unknown_version);
    EXPECT_PROJECT_ERROR((void)repo.resolve("abc"), ProjectErrc::unknown_version);
    EXPECT_PROJECT_ERROR((void)repo.read(std::string(40, '0')), ProjectErrc::unknown_version);

    // A variant named like an id wins over the id.
    repo.set_branch(first.substr(0, 8), third, true);
    EXPECT_EQ(repo.resolve(first.substr(0, 8)), third);
}

TEST_F(StorageRepository, HeadFollowsAVariantOrAVersion) {
    const std::string first = repo.commit(files_with("1.0"), "", note("1"), author, {});
    repo.set_branch("main", first, false);
    repo.set_branch("other", first, true);

    repo.attach_head("other");
    EXPECT_EQ(repo.head().branch, std::optional<std::string>("other"));
    EXPECT_EQ(repo.head().version, std::optional<std::string>(first));

    repo.detach_head(first);
    EXPECT_FALSE(repo.head().branch.has_value());
    EXPECT_EQ(repo.head().version, std::optional<std::string>(first));

    repo.attach_head(utf8(u8"новый"));
    EXPECT_EQ(repo.head().branch, std::optional<std::string>(utf8(u8"новый")));
    EXPECT_FALSE(repo.head().version.has_value());
}

TEST_F(StorageRepository, LogIsNewestFirstAndCanBeLimited) {
    const std::string first = repo.commit(files_with("1.0"), "", note("1"), author, {});
    const std::string second =
        repo.commit(files_with("2.0"), "", note("2"), author, std::vector<std::string>{first});
    const std::string side =
        repo.commit(files_with("9.0"), "", note("side"), author, std::vector<std::string>{first});

    const std::vector<VersionInfo> main_log = repo.log(std::vector<std::string>{second});
    ASSERT_EQ(main_log.size(), 2U);
    EXPECT_EQ(main_log[0].id, second);
    EXPECT_EQ(main_log[1].id, first);
    EXPECT_EQ(repo.log(std::vector<std::string>{second}, 1).size(), 1U);

    const std::vector<VersionInfo> both = repo.log(std::vector<std::string>{second, side});
    ASSERT_EQ(both.size(), 3U);
    EXPECT_EQ(both.back().id, first);
}

TEST_F(StorageRepository, StashesAreNewestFirstAndCanBeDropped) {
    const std::string base = repo.commit(files_with("1.0"), "", note("1"), author, {});
    const std::vector<std::string> parents{base};
    std::vector<std::string> stashed;
    for (int i = 0; i < 12; ++i) {
        stashed.push_back(repo.commit(files_with(std::to_string(i) + ".5"), "",
                                      note("aside", VersionKind::stash), author, parents));
        repo.push_stash(stashed.back());
    }
    std::vector<std::string> expected(stashed.rbegin(), stashed.rend());
    EXPECT_EQ(repo.stashes(), expected);

    repo.drop_stash(1);
    expected.erase(expected.begin() + 1);
    EXPECT_EQ(repo.stashes(), expected);
    EXPECT_PROJECT_ERROR(repo.drop_stash(11), ProjectErrc::unknown_version);
}

TEST_F(StorageRepository, ReachableCoversVariantsLabelsStashesAndHead) {
    const std::string first = repo.commit(files_with("1.0"), "", note("1"), author, {});
    const std::string second =
        repo.commit(files_with("2.0"), "", note("2"), author, std::vector<std::string>{first});
    const std::string labelled =
        repo.commit(files_with("3.0"), "", note("3"), author, std::vector<std::string>{first});
    const std::string stashed = repo.commit(files_with("4.0"), "", note("4", VersionKind::stash),
                                            author, std::vector<std::string>{second});
    const std::string viewed =
        repo.commit(files_with("5.0"), "", note("5"), author, std::vector<std::string>{first});
    const std::string lost =
        repo.commit(files_with("6.0"), "", note("6"), author, std::vector<std::string>{first});
    repo.set_branch("main", second, false);
    repo.create_tag("label", labelled);
    repo.push_stash(stashed);
    repo.detach_head(viewed);

    const std::vector<std::string> found = repo.reachable();
    const std::set<std::string> ids(found.begin(), found.end());
    EXPECT_EQ(ids, (std::set<std::string>{first, second, labelled, stashed, viewed}));
    EXPECT_EQ(found.size(), ids.size());
    EXPECT_FALSE(ids.contains(lost));
}

// --- Pinned results (task 2.5) -------------------------------------------------------------------

TEST_F(StorageRepository, PinnedResultsAreRecordsUnderTheirKeys) {
    const std::string key(64, 'c');
    EXPECT_TRUE(repo.pins().empty());
    EXPECT_FALSE(repo.pin(key).has_value());
    repo.set_pin(key, utf8(u8"{\"files\": \"напряжения\"}\n"));
    repo.set_pin(std::string(64, 'd'), "second");
    EXPECT_EQ(repo.pin(key), std::optional<std::string>(utf8(u8"{\"files\": \"напряжения\"}\n")));
    repo.set_pin(key, "replaced");
    EXPECT_EQ(repo.pins(), (std::map<std::string, std::string>{{key, "replaced"},
                                                               {std::string(64, 'd'), "second"}}));
    // A pin is not a version: walks of the history do not see it.
    EXPECT_TRUE(repo.reachable().empty());
    EXPECT_TRUE(repo.remove_pin(key));
    EXPECT_FALSE(repo.remove_pin(key));
    EXPECT_EQ(repo.pins().size(), 1U);
    EXPECT_PROJECT_ERROR(repo.set_pin("../heads/main", "x"), ProjectErrc::unknown_result);
    EXPECT_PROJECT_ERROR((void)repo.pin(std::string(64, 'C')), ProjectErrc::unknown_result);
}

TEST_F(StorageRepository, AdoptingKeepsHeadVariantsAndPins) {
    const std::string first = repo.commit(files_with("2.0"), "", note("First"), author, {});
    repo.set_branch(utf8(u8"Вариант"), first, true);
    repo.set_pin(std::string(64, 'e'), "record");
    repo.detach_head(first);
    // A history unpacked from a package comes without its configuration.
    fs::remove(dir.path() / ".fem" / "git" / "config");

    Repository::adopt(dir.path());
    // The configuration of a new history on this computer, ignored case included.
    TempDir fresh("fresh history");
    (void)Repository::open_or_create(fresh.path());
    const auto config = [](const fs::path& folder) {
        return io::read_file(folder / ".fem" / "git" / "config").value_or("");
    };
    EXPECT_EQ(config(dir.path()), config(fresh.path()));
    EXPECT_NE(config(dir.path()), "");
    EXPECT_FALSE(fs::exists(dir.path() / ".fem" / "git-probe"));
    std::optional<Repository> again = Repository::open(dir.path());
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->head().branch.has_value());
    EXPECT_EQ(again->head().version, std::optional<std::string>(first));
    EXPECT_EQ(again->branches().at(utf8(u8"Вариант")), first);
    EXPECT_EQ(again->pin(std::string(64, 'e')), std::optional<std::string>("record"));
    EXPECT_EQ(again->read(first).files, files_with("2.0"));
}

}  // namespace fem::storage
