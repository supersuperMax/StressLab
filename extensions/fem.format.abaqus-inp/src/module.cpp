// The native module fem.format.abaqus-inp: the command "inp.import" reads an Abaqus input file
// into the model, "inp.export" writes the model to one and "inp.describe_losses" tells what the
// file would not hold, through the C ABI of the platform (fem/extension_abi.h). What it reads and
// writes: docs/extensions/fem.format.abaqus-inp.md, docs/adr/0024-abaqus-inp-import.md,
// docs/adr/0025-abaqus-inp-export.md.

#include "fem/extension_abi.h"

#include "inp/deck.h"
#include "inp/elements.h"
#include "inp/export.h"
#include "inp/import.h"
#include "inp/model.h"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;

struct State {
    const FemHostApi* host = nullptr;
    Json english = Json::object();  // Strings of l10n/en.json, for error messages of the command.
};

State state;

/// The English text of a problem, for the error of the command (Python, logs); the user sees
/// the problem itself, translated by the platform.
std::string english(const fem::inp::Problem& problem) {
    const auto it = state.english.find(problem.key);
    if (it == state.english.end()) {
        return problem.key;
    }
    std::string text;
    if (it->is_string()) {
        text = it->get<std::string>();
    } else if (it->is_object()) {
        const bool one = problem.args.contains("count") && problem.args["count"] == 1;
        text = it->value(one ? "one" : "other", it->value("other", problem.key));
    }
    for (const auto& [name, value] : problem.args.items()) {
        const std::string placeholder = "{" + name + "}";
        const std::string replacement = value.is_string() ? value.get<std::string>() : value.dump();
        for (std::size_t at = text.find(placeholder); at != std::string::npos;
             at = text.find(placeholder, at + replacement.size())) {
            text.replace(at, placeholder.size(), replacement);
        }
    }
    if (!problem.file.empty()) {
        text = problem.file + (problem.line > 0 ? ":" + std::to_string(problem.line) : "") + ": " +
               text;
    }
    return text;
}

Json problem_json(const fem::inp::Problem& problem) {
    Json json = {{"severity", fem::inp::to_string(problem.severity)},
                 {"message", "%" + problem.key + "%"},
                 {"args", problem.args}};
    if (!problem.file.empty()) {
        json["file"] = problem.file;
    }
    if (problem.line > 0) {
        json["line"] = problem.line;
    }
    return json;
}

FemDataKind data_kind(fem::inp::SetKind kind) {
    switch (kind) {
        case fem::inp::SetKind::node:
            return FEM_DATA_NODE_SET;
        case fem::inp::SetKind::element:
            return FEM_DATA_ELEMENT_SET;
        case fem::inp::SetKind::surface:
            return FEM_DATA_SURFACE;
    }
    return FEM_DATA_NONE;
}

/// Creates the objects of an import in order; false after `set_error`.
bool create(FemCommandCall* call, const FemCommandApi& api, const fem::inp::Import& result,
            std::vector<int64_t>& ids) {
    ids.reserve(result.items.size());
    for (const fem::inp::Item& item : result.items) {
        Json object = {{"type", item.type},
                       {"owner", item.owner},
                       {"name", item.name},
                       {"properties", item.properties},
                       {"unique_name", true}};
        if (item.parent != fem::inp::no_item) {
            object["parent"] = ids[item.parent];
        }
        Json dependencies = Json::array();
        for (const fem::inp::ItemRef target : item.dependencies) {
            dependencies.push_back(ids[target]);
        }
        object["dependencies"] = std::move(dependencies);

        FemObjectData data{static_cast<uint32_t>(sizeof(FemObjectData)), FEM_DATA_NONE, nullptr,
                           nullptr, 0};
        FemMeshData mesh{};
        std::vector<FemElementBlock> blocks;
        if (const auto* m = std::get_if<fem::inp::MeshData>(&item.data)) {
            for (const fem::inp::MeshBlock& block : m->blocks) {
                blocks.push_back({static_cast<uint32_t>(sizeof(FemElementBlock)),
                                  block.shape.c_str(), static_cast<int64_t>(block.ids.size()),
                                  block.ids.data(), block.connectivity.data()});
            }
            mesh = FemMeshData{static_cast<uint32_t>(sizeof(FemMeshData)),
                               static_cast<int64_t>(m->node_ids.size()),
                               m->node_ids.data(),
                               m->x.data(),
                               m->y.data(),
                               m->z.data(),
                               static_cast<int64_t>(blocks.size()),
                               blocks.data()};
            data.kind = FEM_DATA_MESH;
            data.mesh = &mesh;
        } else if (const auto* set = std::get_if<fem::inp::SetData>(&item.data)) {
            data.kind = data_kind(set->kind);
            data.indices = set->indices.data();
            data.count = static_cast<int64_t>(set->kind == fem::inp::SetKind::surface
                                                  ? set->indices.size() / 2
                                                  : set->indices.size());
        }
        int64_t id = 0;
        const std::string text = object.dump();
        if (api.create_object(call, text.c_str(), data.kind == FEM_DATA_NONE ? nullptr : &data,
                              &id) != FEM_OK) {
            api.set_error(call,
                          (item.type + " '" + item.name + "': " + api.last_error(call)).c_str());
            return false;
        }
        ids.push_back(id);
    }
    return true;
}

FemStatus import_command(FemCommandCall* call, void* /*user_data*/) {
    const FemCommandApi& api = *state.host->commands;
    try {
        const Json args = Json::parse(api.arguments(call));
        const std::string path = args.at("path").get<std::string>();
        const std::string encoding_name = args.value("encoding", std::string("auto"));
        const std::optional<fem::inp::Encoding> encoding = fem::inp::parse_encoding(encoding_name);
        if (!encoding) {
            throw fem::inp::InpError({fem::inp::Problem::Severity::error,
                                      "error.unknownEncoding",
                                      {{"encoding", encoding_name}},
                                      {},
                                      0});
        }
        const fem::inp::Import result = fem::inp::import_file(fem::inp::text_path(path), *encoding);
        for (const fem::inp::Problem& problem : result.problems) {
            (void)api.report_problem(call, problem_json(problem).dump().c_str());
        }
        std::vector<int64_t> ids;
        if (!create(call, api, result, ids)) {
            return FEM_ERROR;
        }
        api.set_result(call, std::to_string(ids[result.input]).c_str());
        return FEM_OK;
    } catch (const fem::inp::InpError& e) {
        (void)api.report_problem(call, problem_json(e.problem()).dump().c_str());
        api.set_error(call, english(e.problem()).c_str());
    } catch (const std::exception& e) {
        api.set_error(call, e.what());
    }
    return FEM_ERROR;
}

// --- Export (task 3.3) ---------------------------------------------------------------------------

/// The model of the running command, its bulk data read in place: valid until the call returns.
fem::inp::Model read_model(FemCommandCall* call, const FemCommandApi& api) {
    const char* text = api.read_model(call);
    if (text == nullptr) {
        throw std::runtime_error(std::string("cannot read the model: ") + api.last_error(call));
    }
    const Json json = Json::parse(text);
    fem::inp::Model model;
    model.units = json.value("units", Json::object());
    for (const Json& object : json.at("objects")) {
        fem::inp::ModelObject out;
        out.id = object.at("id").get<std::int64_t>();
        out.type = object.at("type").get<std::string>();
        out.owner = object.at("owner").get<std::string>();
        out.name = object.at("name").get<std::string>();
        out.parent = object.at("parent").get<std::int64_t>();
        out.dependencies = object.at("dependencies").get<std::vector<std::int64_t>>();
        out.properties = object.at("properties");
        out.opaque = object.value("opaque", false);
        const std::string data = object.value("data", std::string("none"));
        if (data != "none" && data != "array") {
            FemObjectData view{static_cast<uint32_t>(sizeof(FemObjectData)), FEM_DATA_NONE, nullptr,
                               nullptr, 0};
            if (api.object_data(call, out.id, &view) != FEM_OK) {
                throw std::runtime_error(std::string("cannot read the data of '") + out.name +
                                         "': " + api.last_error(call));
            }
            const auto count = static_cast<std::size_t>(view.count);
            if (view.kind == FEM_DATA_MESH) {
                const FemMeshData& mesh = *view.mesh;
                const auto nodes = static_cast<std::size_t>(mesh.node_count);
                fem::inp::MeshView meshes{
                    {mesh.node_ids, nodes}, {mesh.x, nodes}, {mesh.y, nodes}, {mesh.z, nodes}, {}};
                for (int64_t b = 0; b < mesh.block_count; ++b) {
                    const FemElementBlock& block = mesh.blocks[b];
                    const auto elements = static_cast<std::size_t>(block.count);
                    const std::optional<int> per_element = fem::inp::shape_nodes(block.type);
                    if (!per_element) {
                        throw std::runtime_error(std::string("unknown element type '") +
                                                 block.type + "'");
                    }
                    meshes.blocks.push_back(
                        {block.type,
                         {block.ids, elements},
                         {block.connectivity, elements * static_cast<std::size_t>(*per_element)}});
                }
                out.data = std::move(meshes);
            } else if (view.kind == FEM_DATA_SURFACE) {
                out.data = fem::inp::SetView{fem::inp::SetKind::surface, {view.indices, count * 2}};
            } else if (view.kind == FEM_DATA_NODE_SET || view.kind == FEM_DATA_ELEMENT_SET) {
                out.data =
                    fem::inp::SetView{view.kind == FEM_DATA_NODE_SET ? fem::inp::SetKind::node
                                                                     : fem::inp::SetKind::element,
                                      {view.indices, count}};
            }
        }
        model.objects.push_back(std::move(out));
    }
    return model;
}

/// The name of the file without its extension, as the import names a mesh after it.
std::string file_stem(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const std::u8string stem = fem::inp::text_path(path).stem().u8string();
    return {stem.begin(), stem.end()};
}

void report(FemCommandCall* call, const FemCommandApi& api,
            const std::vector<fem::inp::Problem>& losses) {
    for (const fem::inp::Problem& loss : losses) {
        (void)api.report_problem(call, problem_json(loss).dump().c_str());
    }
}

FemStatus export_command(FemCommandCall* call, void* /*user_data*/) {
    const FemCommandApi& api = *state.host->commands;
    try {
        const Json args = Json::parse(api.arguments(call));
        const std::string path = args.at("path").get<std::string>();
        const fem::inp::Model model = read_model(call, api);
        const fem::inp::Export result = fem::inp::export_model(model, file_stem(path));
        // Written next to the file first, so that a failed write leaves the old file as it was.
        const std::filesystem::path file = fem::inp::text_path(path);
        std::filesystem::path temporary = file;
        temporary += ".part";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(result.text.data(), static_cast<std::streamsize>(result.text.size()));
            stream.close();
            if (!stream) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw fem::inp::InpError({fem::inp::Problem::Severity::error,
                                          "error.fileUnwritable",
                                          {{"file", path}},
                                          {},
                                          0});
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, file, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw fem::inp::InpError({fem::inp::Problem::Severity::error,
                                      "error.fileUnwritable",
                                      {{"file", path}},
                                      {},
                                      0});
        }
        report(call, api, result.losses);
        api.set_result(call, std::to_string(result.keywords).c_str());
        return FEM_OK;
    } catch (const fem::inp::InpError& e) {
        (void)api.report_problem(call, problem_json(e.problem()).dump().c_str());
        api.set_error(call, english(e.problem()).c_str());
    } catch (const std::exception& e) {
        api.set_error(call, e.what());
    }
    return FEM_ERROR;
}

FemStatus describe_losses_command(FemCommandCall* call, void* /*user_data*/) {
    const FemCommandApi& api = *state.host->commands;
    try {
        const Json args = Json::parse(api.arguments(call));
        const std::string path = args.value("path", std::string());
        const std::vector<fem::inp::Problem> losses =
            fem::inp::describe_losses(read_model(call, api), file_stem(path));
        // As the "Problems" tab shows them, with the English text for scripts.
        Json result = Json::array();
        for (const fem::inp::Problem& loss : losses) {
            Json item = problem_json(loss);
            item["text"] = english(loss);
            result.push_back(std::move(item));
        }
        api.set_result(call, result.dump().c_str());
        return FEM_OK;
    } catch (const std::exception& e) {
        api.set_error(call, e.what());
    }
    return FEM_ERROR;
}

FemStatus activate(const FemHostApi* host, void** module_state) {
    state.host = host;
    *module_state = &state;
    if (host->commands->struct_size < sizeof(FemCommandApi)) {
        host->set_error(host->host, "the platform is too old: it has no read_model");
        return FEM_ERROR;
    }
    try {
        std::ifstream file(fem::inp::text_path(host->extension_path) / "l10n" / "en.json");
        if (file) {
            state.english = Json::parse(file).value("strings", Json::object());
        }
    } catch (const std::exception&) {
        state.english = Json::object();  // The keys stand in for the texts.
    }
    if (host->register_command(host->host, "inp.import", import_command, nullptr) != FEM_OK ||
        host->register_command(host->host, "inp.export", export_command, nullptr) != FEM_OK) {
        return FEM_ERROR;
    }
    return host->register_command(host->host, "inp.describe_losses", describe_losses_command,
                                  nullptr);
}

void deactivate(void* /*module_state*/) {
    state.host = nullptr;
}

const FemExtension extension = {
    static_cast<uint32_t>(sizeof(FemExtension)),
    FEM_EXTENSION_ABI_VERSION,
    activate,
    deactivate,
};

}  // namespace

extern "C" FEM_EXTENSION_EXPORT const FemExtension* fem_extension_entry(void) {
    return &extension;
}
