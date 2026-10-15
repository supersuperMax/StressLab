// Property tests of CLAUDE.md rule 6 for every object type a project holds: for many random
// models, save -> load -> save gives the same bytes and the loaded model equals the saved one.
// A failure prints the seed; rerun one seed with STRESSLAB_ROUND_TRIP_SEED=<seed>.

#include "fem/core/model/object.h"
#include "fem/storage/error.h"
#include "fem/storage/json.h"
#include "fem/storage/project.h"

#include "storage_test_support.h"

#include <functional>
#include <optional>

namespace fem::storage {
namespace {

using namespace testing;
using model::NewObject;
using model::ObjectId;

constexpr const char* module = "test.module";
constexpr int seeds = 200;

/// Builds a random model with objects of one kind into `doc`.
using Scenario = std::function<void(TestDocument& doc, Random& random)>;

std::optional<std::uint64_t> only_seed() {
    if (const std::optional<std::string> text = environment("STRESSLAB_ROUND_TRIP_SEED")) {
        return std::stoull(*text);
    }
    return std::nullopt;
}

ModuleVersions versions(const model::Document& document) {
    ModuleVersions result;
    for (const std::string& owner : document.owners()) {
        result.emplace(owner, owner == model::core_owner ? "0.1.0" : "2.3.4");
    }
    return result;
}

void check_round_trips(const Scenario& build) {
    const std::optional<std::uint64_t> seed_only = only_seed();
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        if (seed_only && seed != *seed_only) {
            continue;
        }
        SCOPED_TRACE("seed " + std::to_string(seed));
        Random random(seed);
        TestDocument doc;
        build(doc, random);

        MemoryBlobStore blobs;
        const ModuleVersions modules = versions(doc.document);
        const ProjectFiles saved = write_project(doc.document, modules, blobs);
        const RoundTrip again(saved, blobs, modules);
        ASSERT_EQ(again.files, saved);
        ASSERT_EQ(describe(again.loaded.document), describe(doc.document));
    }
}

std::shared_ptr<const model::Mesh> add_meshes(TestDocument& doc, Random& random,
                                              std::vector<ObjectId>& meshes) {
    std::shared_ptr<const model::Mesh> last;
    for (int i = random.integer(1, 3); i > 0; --i) {
        last = random.mesh();
        meshes.push_back(doc.edit.create(
            model::mesh_object(random.chance(0.8) ? random.text() + std::to_string(i) : "", last)));
    }
    return last;
}

/// Sets of one kind under random meshes: `make(random, mesh)` gives the set object.
void add_sets(TestDocument& doc, Random& random,
              const std::function<NewObject(Random&, ObjectId, const model::Mesh&, int)>& make) {
    std::vector<ObjectId> meshes;
    add_meshes(doc, random, meshes);
    int index = 0;
    for (const ObjectId mesh : meshes) {
        const auto data = doc.document.data<model::Mesh>(mesh);
        for (int i = random.integer(0, 4); i > 0; --i) {
            doc.edit.create(make(random, mesh, *data, ++index));
        }
    }
}

TEST(StorageRoundTrip, Meshes) {
    check_round_trips([](TestDocument& doc, Random& random) {
        std::vector<ObjectId> meshes;
        add_meshes(doc, random, meshes);
        // The same mesh data under two objects stays shared.
        if (random.chance(0.3)) {
            doc.edit.create(model::mesh_object("copy", doc.document.data<model::Mesh>(meshes[0])));
        }
        if (random.chance(0.5)) {
            doc.edit.set_properties(meshes[0], random.object(2));
        }
    });
}

TEST(StorageRoundTrip, NodeSets) {
    check_round_trips([](TestDocument& doc, Random& random) {
        add_sets(doc, random, [](Random& r, ObjectId mesh, const model::Mesh& data, int i) {
            return model::node_set_object(mesh, "N" + std::to_string(i), r.node_set(data));
        });
    });
}

TEST(StorageRoundTrip, ElementSets) {
    check_round_trips([](TestDocument& doc, Random& random) {
        add_sets(doc, random, [](Random& r, ObjectId mesh, const model::Mesh& data, int i) {
            return model::element_set_object(mesh, "E" + std::to_string(i), r.element_set(data));
        });
    });
}

TEST(StorageRoundTrip, Surfaces) {
    check_round_trips([](TestDocument& doc, Random& random) {
        add_sets(doc, random, [](Random& r, ObjectId mesh, const model::Mesh& data, int i) {
            return model::surface_object(mesh, "S" + std::to_string(i), r.surface(data));
        });
    });
}

/// Objects of modules: a random tree with random properties and dependencies, some removed (ids
/// with gaps, missing dependencies), some renamed, in a unit system of its own.
void build_module_objects(TestDocument& doc, Random& random) {
    doc.document.set_owner_enabled(module, true);
    doc.document.set_owner_enabled("other.module", true);
    std::vector<ObjectId> objects;
    for (int i = random.integer(1, 25); i > 0; --i) {
        NewObject init = model::testing::module_object(
            random.chance(0.8) ? module : "other.module", random.chance(0.5) ? "material" : "Load",
            random.chance(0.7) ? random.text() + "#" + std::to_string(i) : "");
        init.schema_version = static_cast<std::uint32_t>(random.integer(1, 4));
        init.properties = random.object(3);
        if (!objects.empty() && random.chance(0.5)) {
            init.parent = objects[static_cast<std::size_t>(
                random.integer(0, static_cast<int>(objects.size()) - 1))];
        }
        for (int d = random.integer(0, 2); d > 0 && !objects.empty(); --d) {
            init.dependencies.push_back(objects[static_cast<std::size_t>(
                random.integer(0, static_cast<int>(objects.size()) - 1))]);
        }
        objects.push_back(doc.edit.create(std::move(init)));
    }
    for (int i = random.integer(0, 3); i > 0; --i) {
        const ObjectId target = objects[static_cast<std::size_t>(
            random.integer(0, static_cast<int>(objects.size()) - 1))];
        if (doc.document.contains(target)) {
            (void)doc.edit.remove(target);
        }
    }
    if (random.chance(0.5)) {
        doc.edit.set_units(model::UnitSystem::mm_t_s());
    }
}

TEST(StorageRoundTrip, ModuleObjects) {
    check_round_trips(build_module_objects);
}

TEST(StorageRoundTrip, ObjectsOfDisabledModulesAreKeptAsTheyWere) {
    check_round_trips([](TestDocument& doc, Random& random) {
        build_module_objects(doc, random);
        // The modules are gone: their objects are opaque, and still saved byte for byte.
        doc.document.set_owner_enabled(module, false);
        doc.document.set_owner_enabled("other.module", false);
    });
}

TEST(StorageRoundTrip, MixedModelThroughTheFolder) {
    // The same property through files on disk, for a smaller number of models.
    TempDir dir("round-trip");
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        Random random(seed);
        TestDocument doc;
        build_module_objects(doc, random);
        add_sets(doc, random, [](Random& r, ObjectId mesh, const model::Mesh& data, int i) {
            return model::surface_object(mesh, "S" + std::to_string(i), r.surface(data));
        });
        const fs::path folder = dir.path() / ("project " + std::to_string(seed) + ".slproj");
        const ModuleVersions modules = versions(doc.document);
        (void)save_project(folder, doc.document, modules);

        TestDocument loaded;
        loaded.bus().replace_document(load_project(folder).contents);
        ASSERT_EQ(describe(loaded.document), describe(doc.document));
        const SaveStats stats = save_project(folder, loaded.document, modules);
        EXPECT_EQ(stats.written, 0U);
        EXPECT_EQ(stats.removed, 0U);
    }
}

}  // namespace
}  // namespace fem::storage
