// Packages (task 2.5): a project folder in one .slpack and back, with its history or without it;
// the same bytes on every computer; hostile and damaged packages refused; and the exchange of
// packages between the operating systems of the CI.

#include "fem/storage/package.h"

#include "fem/core/command/error.h"
#include "fem/core/command/python_literal.h"
#include "fem/core/io/files.h"
#include "fem/storage/diff.h"
#include "fem/storage/error.h"
#include "fem/storage/json.h"
#include "fem/storage/project.h"
#include "fem/storage/repository.h"
#include "fem/storage/result_cache.h"
#include "fem/storage/session.h"
#include "fem/storage/sha256.h"

#include "project_fixture.h"
#include "storage_test_support.h"
#include <gtest/gtest.h>
#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

const SolverInfo solver{"calculix", "2.22"};
const Author engineer{utf8(u8"Инженер"), "engineer@example.com"};

std::string hash(std::string_view bytes) {
    return sha256_hex(std::as_bytes(std::span(bytes)));
}

/// The names of the entries of a zip file.
std::vector<std::string> entries_of(const fs::path& package) {
    const std::string bytes = read_bytes(package);
    mz_zip_archive zip{};
    EXPECT_TRUE(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0));
    std::vector<std::string> names;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        char name[1024] = {};
        (void)mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        names.emplace_back(name);
    }
    mz_zip_reader_end(&zip);
    return names;
}

/// A zip of these entries as they are, stored without compression.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    EXPECT_TRUE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, bytes] : entries) {
        EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                          MZ_NO_COMPRESSION))
            << name;
    }
    void* data = nullptr;
    std::size_t size = 0;
    EXPECT_TRUE(mz_zip_writer_finalize_heap_archive(&zip, &data, &size));
    std::string result(static_cast<const char*>(data), size);
    mz_zip_writer_end(&zip);
    return result;
}

/// The files of a folder that a package carries: all but the cache and the configuration of git.
std::map<std::string, std::string> packed_files(const fs::path& folder) {
    std::map<std::string, std::string> files = files_below(folder);
    std::erase_if(files, [](const auto& file) {
        return file.first.starts_with(".fem/cache/") || file.first == ".fem/git/config";
    });
    return files;
}

/// The wing of docs/interface-design.md §12: a mesh, the main variant with a label and a later
/// version, the variant "Композитная обшивка" with a pinned result, changes not in the history.
struct Wing {
    TestDocument doc;
    ProjectSession session;
    std::string result_key;
    std::string variant = utf8(u8"Композитная обшивка");
    std::string label = utf8(u8"Отправлено на согласование 12.10");

    explicit Wing(const fs::path& folder, const fs::path& scratch)
        : session(doc.bus(), [](std::string_view id) {
              return id == "fem.demo" ? std::string("1.5.0") : std::string();
          }) {
        namespace mc = command::model_commands;
        doc.document.set_owner_enabled("fem.demo", true);
        ProjectHistory& history = session.history();
        history.set_author(engineer);
        const auto add = [this](const std::string& type, const std::string& name) {
            return ObjectId(static_cast<std::uint64_t>(
                doc.bus()
                    .execute(mc::create, {{"type", type}, {"owner", "fem.demo"}, {"name", name}})
                    .value.as_integer()));
        };
        const auto set = [this](ObjectId id, const std::string& key, command::Value value) {
            doc.bus().execute(mc::set_property, {{"object", id}, {"key", key}, {"value", value}});
        };
        (void)doc.edit.create(model::mesh_object("wing", GridMesh(50, 16, 3).make()));
        const ObjectId skin = add("section", "skin-upper");
        set(skin, "thickness", 2.0);
        const ObjectId material = add("material", utf8(u8"Д16Т"));
        set(material, "young", 72e9);
        session.save_as(folder);
        (void)history.commit(utf8(u8"Крыло: базовый вариант"));
        history.tag(label);

        history.branch(variant);
        history.checkout(variant);
        set(skin, "thickness", 3.0);
        (void)history.commit(utf8(u8"Толщина обшивки 3 мм"));
        const std::string version = history.snapshot("Before solving");
        result_key = session.results().key(solver);
        const fs::path out = scratch / fs::path(u8"расчёт");
        fs::remove_all(out);
        std::string stresses;
        for (int i = 0; i < 40'000; ++i) {
            stresses += std::to_string(i * 7919 % 100'003) + "\n";
        }
        write_bytes(out / fs::path(u8"напряжения.frd"), stresses);
        write_bytes(out / "log" / "solver.dat", " STEP 1\n");
        (void)session.results().put(result_key, version, solver, {}, out);
        session.results().pin(result_key);

        history.checkout("main");
        (void)add("load", "gust-2");
        (void)history.commit(utf8(u8"Порыв gust-2"));
        set(skin, "thickness", 2.5);  // Not in the history: in the journal of the folder.
        session.save();
    }
};

/// What a package made on one computer must give on another.
nlohmann::json expectations(ProjectSession& session, const fs::path& folder,
                            const fs::path& package, const fs::path& model_package,
                            const std::string& result_key) {
    ProjectHistory& history = session.history();
    nlohmann::json result;
    result["branches"] = history.branches();
    result["tags"] = history.tags();
    result["head"] = *history.current_branch();
    nlohmann::json versions = nlohmann::json::array();
    for (const VersionInfo& version : history.log(std::nullopt, true)) {
        versions.push_back(version.id + " " + version.summary);
    }
    result["versions"] = versions;
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [path, bytes] : read_project_files(folder)) {
        files[path] = hash(bytes);
    }
    result["files"] = files;
    result["diff"] = to_json(history.diff("main", utf8(u8"Композитная обшивка")));
    result["result"] = result_key;
    nlohmann::json result_files = nlohmann::json::object();
    const std::optional<ResultInfo> found = session.results().find(result_key);
    for (const ResultFile& file : found->files) {
        result_files[file.path] = file.sha256;
    }
    result["resultFiles"] = result_files;
    result["package"] = hash(read_bytes(package));
    result["modelPackage"] = hash(read_bytes(model_package));
    return result;
}

/// Unpacks the packages of `from` (made by `Wing` on any computer) and checks them against its
/// expected.json.
void check_packages(const fs::path& from) {
    SCOPED_TRACE(io::display(from));
    const nlohmann::json expected = nlohmann::json::parse(read_bytes(from / "expected.json"));
    TempDir dir("unpacked");

    // With the history.
    const fs::path folder = dir.path() / fs::path(u8"крыло от коллеги.slproj");
    TestDocument doc;
    ProjectSession session(doc.bus());
    (void)session.unpack(from / "wing.slpack", folder);
    // Packed again here, the same bytes: nothing of the computer that made it is in it.
    const fs::path again = dir.path() / "again.slpack";
    (void)pack_project(folder, again);
    EXPECT_EQ(hash(read_bytes(again)), expected.at("package").get<std::string>());

    ProjectHistory& history = session.history();
    EXPECT_EQ(nlohmann::json(history.branches()), expected.at("branches"));
    EXPECT_EQ(nlohmann::json(history.tags()), expected.at("tags"));
    EXPECT_EQ(*history.current_branch(), expected.at("head").get<std::string>());
    nlohmann::json versions = nlohmann::json::array();
    for (const VersionInfo& version : history.log(std::nullopt, true)) {
        versions.push_back(version.id + " " + version.summary);
    }
    EXPECT_EQ(versions, expected.at("versions"));
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [path, bytes] : read_project_files(folder)) {
        files[path] = hash(bytes);
    }
    EXPECT_EQ(files, expected.at("files"));
    EXPECT_EQ(hash(read_bytes(folder / fs::path(u8".fem/journal.slpy"))),
              expected.at("journal").get<std::string>());
    EXPECT_EQ(to_json(history.diff("main", utf8(u8"Композитная обшивка"))), expected.at("diff"));
    // The changes not in the history came along.
    EXPECT_TRUE(history.has_changes());

    // The pinned result comes back from the history, byte for byte.
    const std::string key = expected.at("result");
    const std::optional<ResultInfo> result = session.results().find(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->pinned);
    nlohmann::json result_files = nlohmann::json::object();
    for (const ResultFile& file : result->files) {
        result_files[file.path] = hash(read_bytes(
            *result->folder / fs::path(std::u8string(file.path.begin(), file.path.end()))));
    }
    EXPECT_EQ(result_files, expected.at("resultFiles"));
    // Every block the package carries is needed, and every block needed is there.
    EXPECT_EQ(session.clean().removed, 0U);
    // The history works here: the changes become a version, the variants switch.
    const std::size_t versions_before = history.log().size();
    (void)history.commit(utf8(u8"Толщина 2,5 мм у коллеги"));
    EXPECT_EQ(history.log().size(), versions_before + 1);
    history.checkout(utf8(u8"Композитная обшивка"));
    EXPECT_EQ(session.results().list().size(), 1U);

    // Without the history.
    const fs::path model = dir.path() / fs::path(u8"модель.slproj");
    (void)unpack_project(from / "wing-model.slpack", model);
    EXPECT_FALSE(fs::exists(model / ".fem" / "git"));
    EXPECT_FALSE(fs::exists(model / fs::path(u8".fem/journal.slpy")));
    nlohmann::json model_files = nlohmann::json::object();
    for (const auto& [path, bytes] : read_project_files(model)) {
        model_files[path] = hash(bytes);
    }
    EXPECT_EQ(model_files, expected.at("files"));
    EXPECT_NO_THROW((void)load_project(model));
    const fs::path model_again = dir.path() / "model-again.slpack";
    (void)pack_project(model, model_again, {.history = false});
    EXPECT_EQ(hash(read_bytes(model_again)), expected.at("modelPackage").get<std::string>());
}

/// Makes the packages of `Wing` and their expected.json in `to`.
void make_packages(const fs::path& to) {
    TempDir dir("packed");
    const fs::path folder = dir.path() / fs::path(u8"крыло.slproj");
    Wing wing(folder, dir.path());
    fs::create_directories(to);
    const fs::path package = to / "wing.slpack";
    const fs::path model_package = to / "wing-model.slpack";
    (void)wing.session.pack(package);
    // The journal as packed: the next pack saves again, and the line of this one goes to it.
    const std::string journal = hash(read_bytes(folder / fs::path(u8".fem/journal.slpy")));
    (void)wing.session.pack(model_package, false);
    nlohmann::json expected =
        expectations(wing.session, folder, package, model_package, wing.result_key);
    expected["journal"] = journal;
    write_bytes(to / "expected.json", to_canonical_json(expected));
}

}  // namespace

// --- Packing and unpacking -----------------------------------------------------------------------

class StoragePackage : public ProjectFixture {
protected:
    StoragePackage() : ProjectFixture("packages") {}

    fs::path package() const { return dir.path() / fs::path(u8"для коллеги.slpack"); }
    fs::path unpacked() const { return dir.path() / fs::path(u8"распаковано") / "wing.slproj"; }
};

TEST_F(StoragePackage, WithTheHistoryEverythingButTheCacheComesBack) {
    const fs::path scratch = dir.path() / "scratch";
    Wing wing(folder(), scratch);
    // Garbage in the store and a result only in the cache: neither is packed.
    write_bytes(folder() / ".fem" / "objects" / "sha256" / "00" / std::string(62, '0'), "junk");
    (void)wing.session.results().find(wing.result_key);
    ASSERT_TRUE(fs::exists(folder() / fs::path(u8".fem/cache/results")));

    const PackStats packed = wing.session.pack(package());
    EXPECT_GT(packed.files, 10U);
    EXPECT_EQ(packed.package_bytes, fs::file_size(package()));
    for (const std::string& name : entries_of(package())) {
        EXPECT_FALSE(name.starts_with(".fem/cache")) << name;
        EXPECT_NE(name, ".fem/git/config");
        EXPECT_EQ(name.find(std::string(62, '0')), std::string::npos) << name;
    }
    const std::vector<std::string> names = entries_of(package());
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));

    const PackStats unpacked_stats = unpack_project(package(), unpacked());
    EXPECT_EQ(unpacked_stats.files, packed.files);
    std::map<std::string, std::string> expected = packed_files(folder());
    expected.erase(".fem/objects/sha256/00/" + std::string(62, '0'));
    EXPECT_EQ(packed_files(unpacked()), expected);
    EXPECT_TRUE(fs::is_regular_file(unpacked() / ".fem" / "git" / "config"));
}

TEST_F(StoragePackage, WithoutTheHistoryOnlyTheModelAsSaved) {
    const ObjectId mesh = doc.edit.create(model::mesh_object("wing", GridMesh(40, 20, 4).make()));
    session.save_as(folder());
    (void)history().commit("Fine mesh");
    doc.edit.set_data(mesh, GridMesh(10, 5, 2).make());
    const PackStats packed = session.pack(package(), false);

    std::set<std::string> folders;
    for (const std::string& name : entries_of(package())) {
        folders.insert(name.substr(0, name.find('/')));
        EXPECT_FALSE(name.starts_with(".fem/git")) << name;
        EXPECT_NE(name, ".fem/journal.slpy");
    }
    EXPECT_EQ(folders, (std::set<std::string>{".fem", "model", "project.json", "refs"}));
    (void)unpack_project(package(), unpacked());
    EXPECT_EQ(read_project_files(unpacked()), read_project_files(folder()));
    // Only the blocks of the coarse mesh saved last.
    const FolderBlobStore all(folder() / fs::path(u8".fem/objects"));
    const FolderBlobStore carried(unpacked() / fs::path(u8".fem/objects"));
    EXPECT_LT(carried.files().size(), all.files().size());
    TestDocument other;
    ProjectSession opened(other.bus());
    opened.open(unpacked());
    EXPECT_EQ(describe(other.document), describe(doc.document));
    EXPECT_TRUE(opened.history().log().empty());
    (void)packed;
}

TEST_F(StoragePackage, TheSameFolderGivesTheSameBytes) {
    const fs::path scratch = dir.path() / "scratch";
    Wing wing(folder(), scratch);
    const fs::path first = dir.path() / "first.slpack";
    const fs::path second = dir.path() / "second.slpack";
    (void)pack_project(folder(), first);
    (void)pack_project(folder(), second);
    EXPECT_EQ(read_bytes(first), read_bytes(second));
    (void)unpack_project(first, unpacked());
    const fs::path third = dir.path() / "third.slpack";
    (void)pack_project(unpacked(), third);
    EXPECT_EQ(read_bytes(third), read_bytes(first));
}

TEST_F(StoragePackage, SessionPackSavesFirstAndUnpackOpens) {
    const ObjectId skin = committed_project();
    set(skin, 3.0);
    ASSERT_TRUE(session.is_modified());
    take_journal();
    (void)session.pack(dir.path() / fs::path(u8"обмен"));  // ".slpack" is added
    EXPECT_FALSE(session.is_modified());
    const fs::path packed = dir.path() / fs::path(u8"обмен.slpack");
    EXPECT_TRUE(fs::is_regular_file(packed));
    (void)session.pack(dir.path() / "model.slpack", false);
    EXPECT_EQ(take_journal(),
              (std::vector<std::string>{
                  "stresslab.project.pack(" + command::python_string(io::display(packed)) +
                      ", history=True)",
                  "stresslab.project.pack(" +
                      command::python_string(io::display(dir.path() / "model.slpack")) +
                      ", history=False)"}));

    TestDocument other;
    std::vector<std::string> other_journal;
    const Subscription subscription = other.bus().on_journal(
        [&other_journal](std::string_view entry) { other_journal.emplace_back(entry); });
    ProjectSession opened(other.bus());
    const fs::path target = dir.path() / fs::path(u8"у коллеги");
    (void)opened.unpack(packed, target);
    EXPECT_EQ(opened.folder(),
              std::optional<fs::path>(
                  fs::absolute(target).lexically_normal().replace_extension(".slproj")));
    EXPECT_EQ(describe(other.document), describe(doc.document));
    EXPECT_EQ(opened.history().log().size(), 1U);
    EXPECT_TRUE(opened.history().has_changes());  // Thickness 3 is not a version yet.
    EXPECT_FALSE(opened.history().commit("Skin 3 mm").empty());
    ASSERT_FALSE(other_journal.empty());
    EXPECT_EQ(other_journal.front(),
              "stresslab.project.unpack(" + command::python_string(io::display(packed)) + ", " +
                  command::python_string(io::display(dir.path() / fs::path(u8"у коллеги.slproj"))) +
                  ")");
}

TEST_F(StoragePackage, PackNeedsAFolderAndAnIdleBus) {
    (void)add("section", "skin");
    EXPECT_PROJECT_ERRC((void)session.pack(package()), ProjectErrc::no_folder);
    session.save_as(folder());
    command::Transaction transaction = bus().begin_transaction("inside");
    EXPECT_THROW((void)session.pack(package()), command::CommandError);
    transaction.rollback();
    EXPECT_PROJECT_ERRC((void)pack_project(dir.path() / "nothing", package()),
                        ProjectErrc::not_a_project);
}

TEST_F(StoragePackage, AMissingBlockStopsPacking) {
    (void)doc.edit.create(model::mesh_object("wing", GridMesh(40, 20, 4).make()));
    session.save_as(folder());
    const FolderBlobStore blobs(folder() / fs::path(u8".fem/objects"));
    const std::vector<FolderBlobStore::File> files = blobs.files();
    ASSERT_GT(files.size(), 2U);
    fs::remove(files.back().path);  // A piece, or the table.
    try {
        (void)pack_project(folder(), package());
        ADD_FAILURE() << "packed a project without one of its blocks";
    } catch (const ProjectError& error) {
        EXPECT_TRUE(error.code() == ProjectErrc::missing_blob) << error.what();
    }
    EXPECT_FALSE(fs::exists(package()));
}

TEST_F(StoragePackage, CancellingLeavesNothingBehind) {
    const fs::path scratch = dir.path() / "scratch";
    Wing wing(folder(), scratch);
    int calls = 0;
    const Progress cancelling{.report = {}, .cancelled = [&calls] { return ++calls > 20; }};
    EXPECT_PROJECT_ERRC((void)pack_project(folder(), package(), {}, cancelling),
                        ProjectErrc::cancelled);
    EXPECT_FALSE(fs::exists(package()));
    std::vector<double> reported;
    (void)pack_project(folder(), package(), {},
                       {.report = [&reported](double done) { reported.push_back(done); }});
    ASSERT_FALSE(reported.empty());
    EXPECT_TRUE(std::is_sorted(reported.begin(), reported.end()));
    EXPECT_DOUBLE_EQ(reported.back(), 1.0);

    calls = 0;
    EXPECT_PROJECT_ERRC((void)unpack_project(package(), unpacked(), cancelling),
                        ProjectErrc::cancelled);
    EXPECT_FALSE(fs::exists(unpacked()));
    // Nothing half-made beside it either.
    for (const auto& entry : fs::directory_iterator(dir.path())) {
        EXPECT_FALSE(io::display(entry.path().filename()).starts_with(".")) << entry.path();
    }
    for (const auto& entry : fs::directory_iterator(unpacked().parent_path())) {
        ADD_FAILURE() << "left " << io::display(entry.path());
    }
}

TEST_F(StoragePackage, UnpacksOnlyIntoANewOrAnEmptyFolder) {
    (void)committed_project();
    (void)session.pack(package());
    fs::create_directories(unpacked());
    write_bytes(unpacked() / "notes.txt", "mine");
    EXPECT_PROJECT_ERRC((void)unpack_project(package(), unpacked()), ProjectErrc::not_a_project);
    EXPECT_EQ(read_bytes(unpacked() / "notes.txt"), "mine");
    const fs::path file = dir.path() / "file.slproj";
    write_bytes(file, "a file");
    EXPECT_PROJECT_ERRC((void)unpack_project(package(), file), ProjectErrc::not_a_project);
    fs::remove(unpacked() / "notes.txt");
    EXPECT_NO_THROW((void)unpack_project(package(), unpacked()));
    EXPECT_TRUE(is_project_folder(unpacked()));
}

// --- Hostile and damaged packages ----------------------------------------------------------------

class StoragePackageRefusal : public StoragePackage {
protected:
    void expect_refused(const std::string& zip, ProjectErrc code = ProjectErrc::invalid_archive) {
        write_bytes(package(), zip);
        EXPECT_PROJECT_ERRC((void)unpack_project(package(), unpacked()), code);
        EXPECT_FALSE(fs::exists(unpacked()));
        EXPECT_FALSE(fs::exists(dir.path() / "evil"));
    }

    const std::string project = R"({"format": 2})";
};

TEST_F(StoragePackageRefusal, NotAZip) {
    expect_refused("");
    expect_refused("PK\x03\x04 but nothing more");
    expect_refused(std::string(10'000, 'x'));
}

TEST_F(StoragePackageRefusal, PathsOutsideTheFolder) {
    for (const std::string& name :
         {std::string("../evil"), std::string("model/../../evil"), std::string("model/./a.json"),
          std::string("model//a.json"), std::string("C:/evil"), std::string("model/c:a.json"),
          std::string("model\\..\\..\\evil"), std::string("model/con.json"),
          std::string("model/aux"), std::string("model/a."), std::string("model/a "),
          std::string("model/a\x01.json"), std::string("model/a*.json")}) {
        SCOPED_TRACE(name);
        expect_refused(make_zip({{"project.json", project}, {name, "{}"}}));
    }
}

TEST_F(StoragePackageRefusal, AbsolutePaths) {
    // miniz refuses to write a leading '/': the name is made relative, then patched.
    std::string zip = make_zip({{"project.json", project}, {"xevil", "{}"}});
    std::size_t at = 0;
    while ((at = zip.find("xevil", at)) != std::string::npos) {
        zip[at] = '/';
        at += 5;
    }
    expect_refused(zip);
}

TEST_F(StoragePackageRefusal, FilesThatAreNotOfAProject) {
    for (const std::string& name :
         {std::string("other.txt"), std::string("refs/other.json"), std::string(".fem/hooks/x"),
          std::string(".fem/cache/results/a"), std::string(".fem/git/config"),
          std::string(".fem/git/objects/info/alternates")}) {
        SCOPED_TRACE(name);
        expect_refused(make_zip({{"project.json", project}, {name, "x"}}));
    }
    expect_refused(make_zip({{"model/a.json", "{}"}}));  // No project.json.
}

TEST_F(StoragePackageRefusal, NamesThatOneFileSystemMakesOneFile) {
    expect_refused(make_zip({{"project.json", project}, {"Project.json", project}}));
    expect_refused(make_zip({{"project.json", project},
                             {utf8(u8".fem/git/refs/heads/Вариант"), "a"},
                             {utf8(u8".fem/git/refs/heads/вАРИАНТ"), "b"}}));
    expect_refused(
        make_zip({{"project.json", project}, {"model/a", "x"}, {"model/a/b.json", "{}"}}));
    expect_refused(make_zip({{"project.json", project}, {"project.json", project}}));
}

TEST_F(StoragePackageRefusal, Links) {
    std::string zip = make_zip({{"project.json", project}, {"model/link", "../../evil"}});
    // Made on a Unix as a symbolic link: the central directory says so.
    const std::string central = "PK\x01\x02";
    std::size_t at = 0;
    while ((at = zip.find(central, at)) != std::string::npos) {
        const std::size_t name_length = static_cast<unsigned char>(zip[at + 28]) |
                                        (static_cast<unsigned char>(zip[at + 29]) << 8);
        if (zip.compare(at + 46, name_length, "model/link") == 0) {
            zip[at + 5] = 3;  // Made by: Unix.
            const std::uint32_t mode = 0120777U << 16;
            for (int i = 0; i < 4; ++i) {
                zip[at + 38 + i] = static_cast<char>((mode >> (8 * i)) & 0xFF);
            }
        }
        at += 4;
    }
    expect_refused(zip);
}

TEST_F(StoragePackageRefusal, DamagedEntries) {
    std::string zip = make_zip({{"project.json", project}, {"model/a.json", "{\"thickness\": 2}"}});
    const std::size_t at = zip.find("thickness");
    ASSERT_NE(at, std::string::npos);
    zip[at] = 'T';  // The CRC does not match any more.
    expect_refused(zip);
}

TEST_F(StoragePackageRefusal, AProjectOfAnotherFormat) {
    expect_refused(make_zip({{"project.json", R"({"format": 999})"}}),
                   ProjectErrc::unsupported_format);
}

// --- Between computers ---------------------------------------------------------------------------

// The CI packs the wing on each operating system and unpacks the packages of the others
// (.github/workflows/ci.yml, job "slpack-exchange"); without the variables these do nothing.
TEST(StoragePackageExchange, Write) {
    const std::optional<std::string> to = environment("STRESSLAB_SLPACK_EXPORT");
    if (!to) {
        GTEST_SKIP() << "STRESSLAB_SLPACK_EXPORT is not set";
    }
    make_packages(fs::path(std::u8string(to->begin(), to->end())));
}

TEST(StoragePackageExchange, Read) {
    const std::optional<std::string> from = environment("STRESSLAB_SLPACK_IMPORT");
    if (!from) {
        GTEST_SKIP() << "STRESSLAB_SLPACK_IMPORT is not set";
    }
    int read = 0;
    for (const auto& entry :
         fs::recursive_directory_iterator(fs::path(std::u8string(from->begin(), from->end())))) {
        if (entry.path().filename() == "expected.json") {
            check_packages(entry.path().parent_path());
            ++read;
        }
    }
    EXPECT_GT(read, 0) << "no packages in " << *from;
}

// Packages made on Windows, unpacked on every OS of the CI; made again with
// STRESSLAB_UPDATE_GOLDEN=1 when the format of packages changes.
TEST(StoragePackageGolden, PackagesMadeOnAnotherComputer) {
    const fs::path golden = fs::path(FEM_TEST_DATA_DIR) / "packages" / "wing";
    if (environment("STRESSLAB_UPDATE_GOLDEN")) {
        fs::remove_all(golden);
        make_packages(golden);
    }
    check_packages(golden);
}

}  // namespace fem::storage
