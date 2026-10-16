#pragma once

#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/object_data_codec.h"
#include "fem/storage/project_format.h"

#include "../core/model/model_test_support.h"
#include "grid_mesh.h"
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fem::storage::testing {

namespace fs = std::filesystem;
using model::testing::BusEditor;

/// The value of an environment variable, if it is set.
inline std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    std::optional<std::string> result;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

/// A fresh folder with Cyrillic letters and a space in its path, removed afterwards.
class TempDir {
public:
    explicit TempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::path(u8"stresslab тест хранилища") /
                fs::path(std::u8string(name.begin(), name.end()))) {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/// A document with its own bus, for tests that build models through commands.
struct TestDocument {
    model::Document document;
    BusEditor edit{document};

    [[nodiscard]] command::CommandBus& bus() { return edit.bus(); }
};

/// Random values for the property tests: whatever a model may hold, including the numbers that
/// are hard to write (subnormal, -0.0, floats with integral values, the ends of the integer
/// ranges) and text that needs escaping.
class Random {
public:
    explicit Random(std::uint64_t seed) : engine_(seed) {}

    int integer(int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(engine_);
    }
    bool chance(double p) { return std::bernoulli_distribution(p)(engine_); }

    double number() {
        switch (integer(0, 6)) {
            case 0:
                return -0.0;
            case 1:
                return static_cast<double>(integer(-1000, 1000));  // Integral float.
            case 2:
                return std::numeric_limits<double>::denorm_min() * integer(1, 1000);
            case 3:
                return std::uniform_real_distribution<double>(-1.0, 1.0)(engine_);
            case 4:
                return chance(0.5) ? std::numeric_limits<double>::max()
                                   : std::numeric_limits<double>::lowest();
            default: {
                // Any finite bit pattern.
                for (;;) {
                    const double value = std::bit_cast<double>(engine_());
                    if (std::isfinite(value)) {
                        return value;
                    }
                }
            }
        }
    }

    std::string text() {
        static const std::vector<std::string> pieces = {
            "a",    "Z",    "0", " ",     "\"", "\\", "/",   "\n", "\t",    "\r", "\x01",
            "\x1f", "\x7f", "é", "Сталь", "∑",  "😀", " ", "{}", "[1,2]", "-",  "%"};
        std::string result;
        const int count = integer(0, 6);
        for (int i = 0; i < count; ++i) {
            result +=
                pieces[static_cast<std::size_t>(integer(0, static_cast<int>(pieces.size()) - 1))];
        }
        return result;
    }

    nlohmann::json value(int depth) {
        switch (integer(0, depth > 0 ? 10 : 7)) {
            case 0:
                return nullptr;
            case 1:
                return chance(0.5);
            case 2:
                return static_cast<std::int64_t>(engine_());
            case 3:
                return chance(0.5) ? std::numeric_limits<std::int64_t>::min()
                                   : std::numeric_limits<std::int64_t>::max();
            case 4:
                return std::numeric_limits<std::uint64_t>::max();
            case 5:
                return text();
            case 6:
            case 7:
                return number();
            case 8: {
                nlohmann::json array = nlohmann::json::array();
                for (int i = integer(0, 4); i > 0; --i) {
                    array.push_back(value(depth - 1));
                }
                return array;
            }
            default:
                return object(depth - 1);
        }
    }

    nlohmann::json object(int depth) {
        nlohmann::json result = nlohmann::json::object();
        for (int i = integer(0, 5); i > 0; --i) {
            result[text()] = value(depth);
        }
        return result;
    }

    std::shared_ptr<const model::Mesh> mesh() {
        const int nodes = integer(1, 40);
        model::NodeArrays arrays;
        for (int i = 0; i < nodes; ++i) {
            arrays.ids.push_back(static_cast<model::Label>(engine_()));
            arrays.x.push_back(number());
            arrays.y.push_back(number());
            arrays.z.push_back(number());
        }
        std::vector<model::ElementBlock> blocks;
        for (int b = integer(0, 3); b > 0; --b) {
            const auto type = model::all_element_types[static_cast<std::size_t>(
                integer(0, static_cast<int>(model::all_element_types.size()) - 1))];
            const int count = integer(0, 10);
            std::vector<model::Label> ids;
            std::vector<model::Index> connectivity;
            for (int e = 0; e < count; ++e) {
                ids.push_back(static_cast<model::Label>(engine_()));
                for (int n = 0; n < model::nodes_per_element(type); ++n) {
                    connectivity.push_back(integer(0, nodes - 1));
                }
            }
            blocks.emplace_back(type, std::move(ids), std::move(connectivity));
        }
        return std::make_shared<const model::Mesh>(std::move(arrays), std::move(blocks));
    }

    std::shared_ptr<const model::NodeSet> node_set(const model::Mesh& mesh) {
        std::vector<model::Index> nodes;
        for (int i = integer(0, 20); i > 0; --i) {
            nodes.push_back(integer(0, static_cast<int>(mesh.node_count()) - 1));
        }
        return std::make_shared<const model::NodeSet>(std::move(nodes));
    }

    std::shared_ptr<const model::ElementSet> element_set(const model::Mesh& mesh) {
        std::vector<model::Index> elements;
        if (mesh.element_count() > 0) {
            for (int i = integer(0, 20); i > 0; --i) {
                elements.push_back(integer(0, static_cast<int>(mesh.element_count()) - 1));
            }
        }
        return std::make_shared<const model::ElementSet>(std::move(elements));
    }

    std::shared_ptr<const model::Surface> surface(const model::Mesh& mesh) {
        std::vector<model::ElementSide> sides;
        if (mesh.element_count() > 0) {
            for (int i = integer(0, 20); i > 0; --i) {
                const model::Index element = integer(0, static_cast<int>(mesh.element_count()) - 1);
                const auto& info = model::element_type_info(mesh.element_type(element));
                if (!info.sides.empty()) {
                    sides.push_back({element, integer(0, static_cast<int>(info.sides.size()) - 1)});
                }
            }
        }
        return std::make_shared<const model::Surface>(std::move(sides));
    }

private:
    std::mt19937_64 engine_;
};

/// The model of a document as text that tells apart everything a project must keep: 1 from 1.0,
/// -0.0 from 0.0, the order of children, the bytes of bulk data.
inline std::string describe(const model::Document& document) {
    std::string out = "units " + model::to_string(document.units()) + "; next " +
                      std::to_string(document.next_id()) + "; roots";
    for (const model::ObjectId id : document.roots()) {
        out += " " + model::to_string(id);
    }
    out += "\n";
    for (const model::Object& object : document.objects()) {
        out += model::to_string(object.id()) + " " + object.owner() + "/" + object.type() + " v" +
               std::to_string(object.schema_version()) + " \"" + object.name() + "\" parent " +
               model::to_string(object.parent()) + " children";
        for (const model::ObjectId child : object.children()) {
            out += " " + model::to_string(child);
        }
        out += " deps";
        for (const model::ObjectId target : object.dependencies()) {
            out += " " + model::to_string(target);
        }
        out += " props " + object.properties().dump();
        if (object.data()) {
            MemoryBlobStore blobs;
            out += " data " + put_object_data(*object.data(), blobs);
        }
        out += "\n";
    }
    return out;
}

/// Loads `files` into a new document and writes it again.
struct RoundTrip {
    TestDocument loaded;
    ProjectFiles files;

    RoundTrip(const ProjectFiles& original, MemoryBlobStore& blobs, const ModuleVersions& modules) {
        loaded.bus().replace_document(read_project(original, blobs).contents);
        files = write_project(loaded.document, modules, blobs);
    }
};

}  // namespace fem::storage::testing
