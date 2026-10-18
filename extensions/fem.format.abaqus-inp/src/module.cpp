// The native module fem.format.abaqus-inp: the command "inp.import" reads an Abaqus input file
// into the model through the C ABI of the platform (fem/extension_abi.h). What it reads and how:
// docs/extensions/fem.format.abaqus-inp.md, docs/adr/0024-abaqus-inp-import.md.

#include "fem/extension_abi.h"

#include "inp/deck.h"
#include "inp/import.h"
#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <fstream>
#include <string>
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

FemStatus activate(const FemHostApi* host, void** module_state) {
    state.host = host;
    *module_state = &state;
    if (host->commands->struct_size < sizeof(FemCommandApi)) {
        host->set_error(host->host, "the platform is too old: it has no create_object");
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
    return host->register_command(host->host, "inp.import", import_command, nullptr);
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
