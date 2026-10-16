#include "fem/storage/project_format.h"

#include "fem/core/model/object.h"
#include "fem/storage/error.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>

namespace fem::storage {
namespace {

using namespace testing;
using model::ObjectId;

constexpr const char* module = "fem.demo";

/// A mesh with a node set, and an object of a module that depends on the set.
struct SmallProject {
    TestDocument doc;
    ObjectId mesh;
    ObjectId set;
    ObjectId note;

    SmallProject() {
        doc.document.set_owner_enabled(module, true);
        mesh = doc.edit.create(model::mesh_object("Кронштейн", model::testing::small_mesh()));
        set = doc.edit.create(model::node_set_object(
            mesh, "fixed",
            std::make_shared<const model::NodeSet>(std::vector<model::Index>{0, 1})));
        model::NewObject init = model::testing::module_object(module, "note", "Заметка", {}, {set});
        init.properties = {{"text", "Привет"}, {"count", 2}, {"scale", 1.0}};
        note = doc.edit.create(std::move(init));
    }
};

ProjectError read_error(const ProjectFiles& files, const BlobStore& blobs) {
    try {
        (void)read_project(files, blobs);
    } catch (const ProjectError& error) {
        return error;
    }
    ADD_FAILURE() << "the project was read";
    return ProjectError(ProjectErrc::io, "none");
}

TEST(StorageProjectFormat, FilesOfASmallProject) {
    SmallProject project;
    MemoryBlobStore blobs;
    const ProjectFiles files =
        write_project(project.doc.document, {{"fem.core", "0.1.0"}, {module, "1.2.0"}}, blobs);

    std::vector<std::string> paths;
    for (const auto& [path, text] : files) {
        paths.push_back(path);
    }
    EXPECT_EQ(paths, (std::vector<std::string>{"model/ext/fem.demo/note/3.json",
                                               "model/mesh/1.json", "model/node_set/2.json",
                                               "project.json", "refs/blobs.json"}));
    EXPECT_EQ(files.at("project.json"),
              "{\n"
              "  \"format\": 2,\n"
              "  \"modules\": {\n"
              "    \"fem.core\": \"0.1.0\",\n"
              "    \"fem.demo\": \"1.2.0\"\n"
              "  },\n"
              "  \"nextObjectId\": 4,\n"
              "  \"units\": {\n"
              "    \"length\": \"m\",\n"
              "    \"mass\": \"kg\",\n"
              "    \"temperature\": \"K\",\n"
              "    \"time\": \"s\"\n"
              "  }\n"
              "}\n");
    EXPECT_EQ(files.at("model/ext/fem.demo/note/3.json"),
              "{\n"
              "  \"dependencies\": [\n"
              "    2\n"
              "  ],\n"
              "  \"id\": 3,\n"
              "  \"name\": \"Заметка\",\n"
              "  \"owner\": \"fem.demo\",\n"
              "  \"parent\": null,\n"
              "  \"properties\": {\n"
              "    \"count\": 2,\n"
              "    \"scale\": 1.0,\n"
              "    \"text\": \"Привет\"\n"
              "  },\n"
              "  \"schemaVersion\": 1,\n"
              "  \"type\": \"note\"\n"
              "}\n");
    EXPECT_EQ(files.at("model/node_set/2.json"),
              "{\n"
              "  \"data\": {\n"
              "    \"kind\": \"node_set\",\n"
              "    \"size\": 2\n"
              "  },\n"
              "  \"dependencies\": [],\n"
              "  \"id\": 2,\n"
              "  \"name\": \"fixed\",\n"
              "  \"owner\": \"fem.core\",\n"
              "  \"parent\": 1,\n"
              "  \"properties\": {},\n"
              "  \"schemaVersion\": 1,\n"
              "  \"type\": \"node_set\"\n"
              "}\n");
    MemoryBlobStore expected;
    const std::string mesh_ref = put_object_data(*model::testing::small_mesh(), expected);
    const std::string set_ref = put_object_data(model::NodeSet({0, 1}), expected);
    EXPECT_EQ(files.at("refs/blobs.json"),
              "{\n  \"1\": \"" + mesh_ref + "\",\n  \"2\": \"" + set_ref + "\"\n}\n");
    EXPECT_EQ(blobs.size(), 2U);
}

TEST(StorageProjectFormat, ModulesOfObjectsWithoutAKnownVersionAreWrittenEmpty) {
    SmallProject project;
    MemoryBlobStore blobs;
    const ProjectFiles files = write_project(project.doc.document, {{"unused", "9.9.9"}}, blobs);
    const nlohmann::json json = nlohmann::json::parse(files.at("project.json"));
    EXPECT_EQ(json.at("modules"), (nlohmann::json{{"fem.core", ""}, {"fem.demo", ""}}));
}

TEST(StorageProjectFormat, ReadsBackWhatItWrote) {
    SmallProject project;
    MemoryBlobStore blobs;
    const ProjectFiles files = write_project(project.doc.document, {{module, "1.2.0"}}, blobs);
    const LoadedProject loaded = read_project(files, blobs);
    EXPECT_EQ(loaded.contents.next_id, 4U);
    EXPECT_EQ(loaded.contents.objects.size(), 3U);
    EXPECT_EQ(loaded.modules.at(module), "1.2.0");
    EXPECT_EQ(loaded.files.at(ObjectId(3)), "model/ext/fem.demo/note/3.json");

    TestDocument copy;
    copy.bus().replace_document(loaded.contents);
    EXPECT_EQ(describe(copy.document), describe(project.doc.document));
    // The module is not enabled in the copy: its object is opaque and stays as it was.
    EXPECT_TRUE(copy.document.is_opaque(ObjectId(3)));
}

TEST(StorageProjectFormat, PathsArePortable) {
    EXPECT_EQ(object_file("fem.core", "mesh", ObjectId(7)), "model/mesh/7.json");
    EXPECT_EQ(object_file("fem.model.structural", "material", ObjectId(12)),
              "model/ext/fem.model.structural/material/12.json");
    EXPECT_EQ(object_file("my mod", "Нагрузка/2", ObjectId(1)),
              "model/ext/my%20mod/%D0%9D%D0%B0%D0%B3%D1%80%D1%83%D0%B7%D0%BA%D0%B0%2F2/1.json");
    EXPECT_EQ(object_file("..", "aux", ObjectId(1)), "model/ext/%2E%2E/%61ux/1.json");
    EXPECT_EQ(object_file("x.", "Con.v2", ObjectId(1)), "model/ext/x%2E/%43on.v2/1.json");
    EXPECT_EQ(object_file("a", ".hidden", ObjectId(1)), "model/ext/a/%2Ehidden/1.json");
}

class StorageProjectFormatErrors : public ::testing::Test {
protected:
    StorageProjectFormatErrors() { files = write_project(project.doc.document, {}, blobs); }

    void expect_error(ProjectErrc code, const std::string& file) {
        const ProjectError error = read_error(files, blobs);
        EXPECT_EQ(to_string(error.code()), to_string(code)) << error.what();
        EXPECT_EQ(error.file(), file) << error.what();
    }

    void edit(const std::string& file, const std::function<void(nlohmann::json&)>& change) {
        nlohmann::json json = nlohmann::json::parse(files.at(file));
        change(json);
        files[file] = json.dump();
    }

    SmallProject project;
    MemoryBlobStore blobs;
    ProjectFiles files;
};

TEST_F(StorageProjectFormatErrors, NoProjectFile) {
    files.erase("project.json");
    expect_error(ProjectErrc::not_a_project, "project.json");
}

TEST_F(StorageProjectFormatErrors, LaterFormat) {
    edit("project.json", [](nlohmann::json& json) {
        json["format"] = 3;
        json["somethingNew"] = true;
    });
    expect_error(ProjectErrc::unsupported_format, "project.json");
}

TEST_F(StorageProjectFormatErrors, FormatOfTask21IsNotRead) {
    edit("project.json", [](nlohmann::json& json) { json["format"] = 1; });
    expect_error(ProjectErrc::unsupported_format, "project.json");
}

TEST_F(StorageProjectFormatErrors, BlobRootsAreTheBlocksOfRefs) {
    const std::vector<std::string> roots = read_blob_roots(files);
    const nlohmann::json refs = nlohmann::json::parse(files.at("refs/blobs.json"));
    ASSERT_EQ(roots.size(), 2U);
    EXPECT_TRUE(std::is_sorted(roots.begin(), roots.end()));
    for (const auto& [id, ref] : refs.items()) {
        EXPECT_TRUE(std::find(roots.begin(), roots.end(), ref.get<std::string>()) != roots.end())
            << id;
    }
    // A project of another format is not cleaned.
    edit("project.json", [](nlohmann::json& json) { json["format"] = 3; });
    EXPECT_THROW((void)read_blob_roots(files), ProjectError);
}

TEST_F(StorageProjectFormatErrors, BrokenJsonNamesTheLine) {
    files["model/mesh/1.json"] = "{\n  \"id\": 1,\n  oops\n}\n";
    const ProjectError error = read_error(files, blobs);
    EXPECT_EQ(error.code(), ProjectErrc::invalid_file);
    EXPECT_EQ(error.file(), "model/mesh/1.json");
    EXPECT_EQ(error.line(), 3);
}

TEST_F(StorageProjectFormatErrors, UnknownUnit) {
    edit("project.json", [](nlohmann::json& json) { json["units"]["length"] = "furlong"; });
    expect_error(ProjectErrc::invalid_file, "project.json");
}

TEST_F(StorageProjectFormatErrors, UnknownMember) {
    edit("model/ext/fem.demo/note/3.json", [](nlohmann::json& json) { json["colour"] = "red"; });
    expect_error(ProjectErrc::invalid_file, "model/ext/fem.demo/note/3.json");
}

TEST_F(StorageProjectFormatErrors, MissingMember) {
    edit("model/mesh/1.json", [](nlohmann::json& json) { json.erase("owner"); });
    expect_error(ProjectErrc::invalid_file, "model/mesh/1.json");
}

TEST_F(StorageProjectFormatErrors, FileNameDoesNotMatchTheId) {
    files["model/mesh/5.json"] = files.at("model/mesh/1.json");
    files.erase("model/mesh/1.json");
    expect_error(ProjectErrc::invalid_file, "model/mesh/5.json");
}

TEST_F(StorageProjectFormatErrors, SameObjectTwice) {
    files["model/elsewhere/1.json"] = files.at("model/mesh/1.json");
    expect_error(ProjectErrc::invalid_file, "model/mesh/1.json");
}

TEST_F(StorageProjectFormatErrors, DataWithoutABlock) {
    edit("refs/blobs.json", [](nlohmann::json& json) { json.erase("2"); });
    expect_error(ProjectErrc::invalid_file, "model/node_set/2.json");
}

TEST_F(StorageProjectFormatErrors, BlockWithoutAnObject) {
    edit("refs/blobs.json", [](nlohmann::json& json) { json["3"] = json["2"]; });
    expect_error(ProjectErrc::invalid_file, "model/ext/fem.demo/note/3.json");
    edit("refs/blobs.json", [](nlohmann::json& json) {
        json.erase("3");
        json["9"] = json["2"];
    });
    expect_error(ProjectErrc::invalid_file, "refs/blobs.json");
}

TEST_F(StorageProjectFormatErrors, MissingBlock) {
    MemoryBlobStore empty;
    const ProjectError error = read_error(files, empty);
    EXPECT_EQ(error.code(), ProjectErrc::missing_blob);
}

TEST_F(StorageProjectFormatErrors, DescriptionDoesNotMatchTheBlock) {
    edit("model/mesh/1.json", [](nlohmann::json& json) { json["data"]["nodes"] = 6; });
    expect_error(ProjectErrc::invalid_file, "model/mesh/1.json");
}

TEST_F(StorageProjectFormatErrors, NegativeId) {
    edit("model/mesh/1.json", [](nlohmann::json& json) { json["parent"] = -1; });
    expect_error(ProjectErrc::invalid_file, "model/mesh/1.json");
}

TEST_F(StorageProjectFormatErrors, InvalidModelIsLeftToTheDocument) {
    // Well-formed files, but the note depends on itself: reading succeeds, loading does not.
    edit("model/ext/fem.demo/note/3.json",
         [](nlohmann::json& json) { json["dependencies"] = {3}; });
    const LoadedProject loaded = read_project(files, blobs);
    TestDocument doc;
    EXPECT_MODEL_ERROR(doc.bus().replace_document(loaded.contents),
                       model::ModelErrc::invalid_dependency);
}

}  // namespace
}  // namespace fem::storage
