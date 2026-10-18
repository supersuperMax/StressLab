#include "native_runtime.h"

#include "fem/core/command/command.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"
#include "fem/core/model/object.h"
#include "fem/core/model/units.h"
#include "fem/extension_abi.h"

#include "files.h"
#include "json_value.h"
#include "shared_library.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace fem::extension::detail {
class NativeInstance;
}  // namespace fem::extension::detail

// The opaque types of the C ABI.

struct FemHost {
    fem::extension::detail::NativeInstance* instance = nullptr;
};

struct FemCommandCall {
    fem::command::CommandContext* context = nullptr;
    std::string arguments;
    std::optional<std::string> result;
    std::string error;
    std::string last_error;
    std::deque<std::string> results;  // Results of `execute`, valid until the call returns.
    // What `read_model` and `object_data` gave out, valid until the call returns.
    std::string model;
    std::deque<std::shared_ptr<const fem::model::ObjectData>> data;
    std::deque<FemMeshData> meshes;
    std::deque<std::vector<FemElementBlock>> blocks;
    std::deque<std::string> type_names;
    std::deque<std::vector<std::int32_t>> sides;
};

namespace fem::extension::detail {

namespace {

[[noreturn]] void fail_activation(const std::string& message) {
    throw ExtensionError(ExtensionErrc::activation_failed, message);
}

/// Libraries loaded by this process, by path; never unloaded.
std::shared_ptr<SharedLibrary> load_library(const std::filesystem::path& file) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::shared_ptr<SharedLibrary>> loaded;
    const std::lock_guard lock(mutex);
    const std::filesystem::path key = std::filesystem::absolute(file).lexically_normal();
    if (const auto it = loaded.find(key); it != loaded.end()) {
        return it->second;
    }
    std::shared_ptr<SharedLibrary> library = SharedLibrary::open(key);
    loaded.emplace(key, library);
    return library;
}

// --- FemCommandApi -------------------------------------------------------------------------------

const char* call_command_id(const FemCommandCall* call) noexcept {
    return call->context->spec().id.c_str();
}

const char* call_arguments(const FemCommandCall* call) noexcept {
    return call->arguments.c_str();
}

FemStatus call_execute(FemCommandCall* call, const char* command_id, const char* arguments,
                       const char** result) noexcept {
    if (result != nullptr) {
        *result = nullptr;
    }
    try {
        if (command_id == nullptr) {
            throw ExtensionError(ExtensionErrc::command_failed, "execute needs a command id");
        }
        command::Value::Dict args;
        if (arguments != nullptr) {
            command::Value value = from_json(nlohmann::json::parse(arguments));
            if (value.kind() == command::Value::Kind::dict) {
                args = std::move(value.as_dict());
            } else if (!value.is_none()) {
                throw ExtensionError(
                    ExtensionErrc::command_failed,
                    "the arguments of '" + std::string(command_id) + "' must be a JSON object");
            }
        }
        const command::CommandResult nested = call->context->execute(command_id, std::move(args));
        call->results.push_back(to_json(nested.value).dump());
        call->last_error.clear();
        if (result != nullptr) {
            *result = call->results.back().c_str();
        }
        return FEM_OK;
    } catch (const std::exception& e) {
        call->last_error = e.what();
    } catch (...) {
        call->last_error = "unknown error";
    }
    return FEM_ERROR;
}

const char* call_last_error(const FemCommandCall* call) noexcept {
    return call->last_error.c_str();
}

void call_set_result(FemCommandCall* call, const char* result) noexcept {
    try {
        call->result = std::string(result != nullptr ? result : "null");
    } catch (...) {
        call->error = "out of memory";
    }
}

void call_set_error(FemCommandCall* call, const char* message) noexcept {
    try {
        call->error = message != nullptr ? message : "";
    } catch (...) {
    }
}

[[noreturn]] void bad_call(const std::string& message) {
    throw ExtensionError(ExtensionErrc::command_failed, message);
}

/// A count of array entries the module passed; `what` names it in the error.
std::size_t checked_count(std::int64_t count, std::string_view what) {
    if (count < 0 || static_cast<std::uint64_t>(count) >
                         static_cast<std::uint64_t>(std::numeric_limits<model::Index>::max())) {
        bad_call("invalid " + std::string(what) + " count " + std::to_string(count));
    }
    return static_cast<std::size_t>(count);
}

template <class T>
std::vector<T> copy_array(const T* values, std::size_t count, std::string_view what) {
    if (count > 0 && values == nullptr) {
        bad_call(std::string(what) + " is NULL");
    }
    return count > 0 ? std::vector<T>(values, values + count) : std::vector<T>();
}

std::shared_ptr<const model::Mesh> mesh_from_abi(const FemMeshData* data) {
    if (data == nullptr || data->struct_size < sizeof(FemMeshData)) {
        bad_call("FEM_DATA_MESH needs a FemMeshData");
    }
    const std::size_t nodes = checked_count(data->node_count, "node");
    model::NodeArrays arrays;
    arrays.ids = copy_array<model::Label>(data->node_ids, nodes, "node_ids");
    arrays.x = copy_array(data->x, nodes, "x");
    arrays.y = copy_array(data->y, nodes, "y");
    arrays.z = copy_array(data->z, nodes, "z");
    const std::size_t count = checked_count(data->block_count, "block");
    if (count > 0 && data->blocks == nullptr) {
        bad_call("blocks is NULL");
    }
    std::vector<model::ElementBlock> blocks;
    blocks.reserve(count);
    for (std::size_t b = 0; b < count; ++b) {
        const FemElementBlock& block = data->blocks[b];
        if (block.struct_size < sizeof(FemElementBlock)) {
            bad_call("FemElementBlock " + std::to_string(b) + " is too small");
        }
        const std::optional<model::ElementType> type =
            block.type != nullptr ? model::parse_element_type(block.type) : std::nullopt;
        if (!type) {
            bad_call("unknown element type '" + std::string(block.type ? block.type : "") + "'");
        }
        const std::size_t elements = checked_count(block.count, "element");
        const auto per_element = static_cast<std::size_t>(model::nodes_per_element(*type));
        blocks.emplace_back(
            *type, copy_array<model::Label>(block.ids, elements, "ids"),
            copy_array<model::Index>(block.connectivity, elements * per_element, "connectivity"));
    }
    return std::make_shared<const model::Mesh>(std::move(arrays), std::move(blocks));
}

std::shared_ptr<const model::ObjectData> data_from_abi(const FemObjectData* data) {
    if (data == nullptr) {
        return nullptr;
    }
    if (data->struct_size < sizeof(FemObjectData)) {
        bad_call("FemObjectData is too small");
    }
    switch (data->kind) {
        case FEM_DATA_NONE:
            return nullptr;
        case FEM_DATA_MESH:
            return mesh_from_abi(data->mesh);
        case FEM_DATA_NODE_SET:
            return std::make_shared<const model::NodeSet>(copy_array<model::Index>(
                data->indices, checked_count(data->count, "index"), "indices"));
        case FEM_DATA_ELEMENT_SET:
            return std::make_shared<const model::ElementSet>(copy_array<model::Index>(
                data->indices, checked_count(data->count, "index"), "indices"));
        case FEM_DATA_SURFACE: {
            const std::size_t count = checked_count(data->count, "side");
            const std::vector<model::Index> pairs =
                copy_array<model::Index>(data->indices, count * 2, "indices");
            std::vector<model::ElementSide> sides(count);
            for (std::size_t i = 0; i < count; ++i) {
                sides[i] = {pairs[2 * i], pairs[2 * i + 1]};
            }
            return std::make_shared<const model::Surface>(std::move(sides));
        }
        default:
            bad_call("unknown data kind " + std::to_string(data->kind));
    }
}

model::ObjectId object_id(const nlohmann::json& value, std::string_view what) {
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
        bad_call("'" + std::string(what) + "' must be an object id");
    }
    return model::ObjectId(value.get<std::uint64_t>());
}

model::NewObject new_object_from_json(const nlohmann::json& object) {
    if (!object.is_object()) {
        bad_call("the object must be a JSON object");
    }
    const auto text = [&](std::string_view key, bool required) {
        const auto it = object.find(key);
        if (it == object.end() || it->is_null()) {
            if (required) {
                bad_call("the object needs '" + std::string(key) + "'");
            }
            return std::string();
        }
        if (!it->is_string()) {
            bad_call("'" + std::string(key) + "' of the object must be text");
        }
        return it->get<std::string>();
    };
    model::NewObject init;
    init.type = text("type", true);
    init.owner = text("owner", true);
    init.name = text("name", false);
    if (const auto it = object.find("schema_version"); it != object.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<std::int64_t>() < 1 ||
            it->get<std::int64_t>() > std::numeric_limits<std::uint32_t>::max()) {
            bad_call("'schema_version' must be a positive integer");
        }
        init.schema_version = it->get<std::uint32_t>();
    }
    if (const auto it = object.find("properties"); it != object.end() && !it->is_null()) {
        if (!it->is_object()) {
            bad_call("'properties' of the object must be an object");
        }
        init.properties = *it;
    }
    if (const auto it = object.find("parent"); it != object.end() && !it->is_null()) {
        init.parent = object_id(*it, "parent");
    }
    if (const auto it = object.find("dependencies"); it != object.end() && !it->is_null()) {
        if (!it->is_array()) {
            bad_call("'dependencies' of the object must be a list of object ids");
        }
        for (const nlohmann::json& target : *it) {
            init.dependencies.push_back(object_id(target, "dependencies"));
        }
    }
    return init;
}

FemStatus call_create_object(FemCommandCall* call, const char* object, const FemObjectData* data,
                             int64_t* id) noexcept {
    try {
        if (object == nullptr) {
            bad_call("create_object needs the object");
        }
        const nlohmann::json json = nlohmann::json::parse(object);
        model::NewObject init = new_object_from_json(json);
        init.data = data_from_abi(data);
        if (const auto unique = json.find("unique_name");
            unique != json.end() && unique->is_boolean() && unique->get<bool>() &&
            !init.name.empty()) {
            // "Steel", then "Steel-2", "Steel-3"...: a second import into the same model.
            const model::Document& document = call->context->document();
            const std::string name = init.name;
            for (int n = 2; document.find_child(init.parent, init.type, init.name); ++n) {
                init.name = name + "-" + std::to_string(n);
            }
        }
        const model::ObjectId created = call->context->editor().create(std::move(init));
        if (id != nullptr) {
            *id = static_cast<int64_t>(created.value());
        }
        call->last_error.clear();
        return FEM_OK;
    } catch (const std::exception& e) {
        call->last_error = e.what();
    } catch (...) {
        call->last_error = "unknown error";
    }
    return FEM_ERROR;
}

command::Problem problem_from_json(const nlohmann::json& json) {
    if (!json.is_object()) {
        bad_call("the problem must be a JSON object");
    }
    command::Problem problem;
    const auto message = json.find("message");
    if (message == json.end() || !message->is_string()) {
        bad_call("the problem needs a 'message' text");
    }
    problem.message = message->get<std::string>();
    if (const auto it = json.find("severity"); it != json.end() && !it->is_null()) {
        using Severity = command::Problem::Severity;
        bool known = false;
        for (const Severity severity : {Severity::info, Severity::warning, Severity::error}) {
            if (it->is_string() && it->get<std::string>() == command::to_string(severity)) {
                problem.severity = severity;
                known = true;
            }
        }
        if (!known) {
            bad_call("'severity' of the problem must be \"info\", \"warning\" or \"error\"");
        }
    }
    if (const auto it = json.find("args"); it != json.end() && !it->is_null()) {
        if (!it->is_object()) {
            bad_call("'args' of the problem must be an object");
        }
        for (const auto& [name, value] : it->items()) {
            if (value.is_string()) {
                problem.args.emplace(name, value.get<std::string>());
            } else if (value.is_number_integer()) {
                problem.args.emplace(name, value.get<std::int64_t>());
            } else if (value.is_number()) {
                problem.args.emplace(name, value.get<double>());
            } else {
                bad_call("argument '" + name + "' of the problem must be text or a number");
            }
        }
    }
    if (const auto it = json.find("file"); it != json.end() && !it->is_null()) {
        if (!it->is_string()) {
            bad_call("'file' of the problem must be text");
        }
        problem.file = it->get<std::string>();
    }
    if (const auto it = json.find("line"); it != json.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<std::int64_t>() < 0) {
            bad_call("'line' of the problem must be a line number");
        }
        problem.line = it->get<std::int64_t>();
    }
    return problem;
}

FemStatus call_report_problem(FemCommandCall* call, const char* problem) noexcept {
    try {
        if (problem == nullptr) {
            bad_call("report_problem needs the problem");
        }
        call->context->report(problem_from_json(nlohmann::json::parse(problem)));
        call->last_error.clear();
        return FEM_OK;
    } catch (const std::exception& e) {
        call->last_error = e.what();
    } catch (...) {
        call->last_error = "unknown error";
    }
    return FEM_ERROR;
}

const char* data_name(const model::ObjectData* data) {
    if (data == nullptr) {
        return "none";
    }
    if (dynamic_cast<const model::Mesh*>(data) != nullptr) {
        return "mesh";
    }
    if (dynamic_cast<const model::NodeSet*>(data) != nullptr) {
        return "node_set";
    }
    if (dynamic_cast<const model::ElementSet*>(data) != nullptr) {
        return "element_set";
    }
    if (dynamic_cast<const model::Surface*>(data) != nullptr) {
        return "surface";
    }
    return "array";
}

void add_subtree(const model::Document& document, model::ObjectId id, nlohmann::json& objects) {
    const model::Object& object = document.get(id);
    nlohmann::json dependencies = nlohmann::json::array();
    for (const model::ObjectId target : object.dependencies()) {
        dependencies.push_back(target.value());
    }
    objects.push_back({{"id", id.value()},
                       {"type", object.type()},
                       {"owner", object.owner()},
                       {"schema_version", object.schema_version()},
                       {"name", object.name()},
                       {"parent", object.parent().is_valid() ? object.parent().value() : 0},
                       {"dependencies", std::move(dependencies)},
                       {"properties", object.properties()},
                       {"data", data_name(object.data().get())},
                       {"opaque", document.is_opaque(object)}});
    for (const model::ObjectId child : object.children()) {
        add_subtree(document, child, objects);
    }
}

const char* call_read_model(FemCommandCall* call) noexcept {
    try {
        const model::Document& document = call->context->document();
        const model::UnitSystem& units = document.units();
        nlohmann::json objects = nlohmann::json::array();
        for (const model::ObjectId root : document.roots()) {
            add_subtree(document, root, objects);
        }
        const nlohmann::json json = {{"units",
                                      {{"length", model::symbol(units.length)},
                                       {"mass", model::symbol(units.mass)},
                                       {"time", model::symbol(units.time)},
                                       {"temperature", model::symbol(units.temperature)}}},
                                     {"objects", std::move(objects)}};
        call->model = json.dump();
        call->last_error.clear();
        return call->model.c_str();
    } catch (const std::exception& e) {
        call->last_error = e.what();
    } catch (...) {
        call->last_error = "unknown error";
    }
    return nullptr;
}

FemStatus call_object_data(FemCommandCall* call, int64_t id, FemObjectData* out) noexcept {
    try {
        if (out == nullptr || out->struct_size < sizeof(FemObjectData)) {
            bad_call("object_data needs a FemObjectData");
        }
        if (id <= 0) {
            bad_call("object_data needs an object id");
        }
        const model::Object* object =
            call->context->document().find(model::ObjectId(static_cast<std::uint64_t>(id)));
        if (object == nullptr) {
            bad_call("no object " + std::to_string(id));
        }
        out->kind = FEM_DATA_NONE;
        out->mesh = nullptr;
        out->indices = nullptr;
        out->count = 0;
        const std::shared_ptr<const model::ObjectData>& data = object->data();
        if (const auto* mesh = dynamic_cast<const model::Mesh*>(data.get())) {
            std::vector<FemElementBlock>& blocks = call->blocks.emplace_back();
            for (const model::ElementBlock& block : mesh->blocks()) {
                const std::string& type =
                    call->type_names.emplace_back(model::to_string(block.type()));
                blocks.push_back({static_cast<std::uint32_t>(sizeof(FemElementBlock)), type.c_str(),
                                  static_cast<int64_t>(block.size()), block.ids().data(),
                                  block.connectivity().data()});
            }
            out->mesh = &call->meshes.emplace_back(
                FemMeshData{static_cast<std::uint32_t>(sizeof(FemMeshData)),
                            static_cast<int64_t>(mesh->node_count()), mesh->node_ids().data(),
                            mesh->x().data(), mesh->y().data(), mesh->z().data(),
                            static_cast<int64_t>(blocks.size()), blocks.data()});
            out->kind = FEM_DATA_MESH;
        } else if (const auto* nodes = dynamic_cast<const model::NodeSet*>(data.get())) {
            out->kind = FEM_DATA_NODE_SET;
            out->indices = nodes->nodes().data();
            out->count = static_cast<int64_t>(nodes->size());
        } else if (const auto* elements = dynamic_cast<const model::ElementSet*>(data.get())) {
            out->kind = FEM_DATA_ELEMENT_SET;
            out->indices = elements->elements().data();
            out->count = static_cast<int64_t>(elements->size());
        } else if (const auto* surface = dynamic_cast<const model::Surface*>(data.get())) {
            // Pairs are copied: the C side reads int32_t pairs, not ElementSide.
            std::vector<std::int32_t>& pairs = call->sides.emplace_back();
            pairs.reserve(surface->size() * 2);
            for (const model::ElementSide& side : surface->sides()) {
                pairs.push_back(side.element);
                pairs.push_back(side.side);
            }
            out->kind = FEM_DATA_SURFACE;
            out->indices = pairs.data();
            out->count = static_cast<int64_t>(surface->size());
        }
        if (out->kind != FEM_DATA_NONE) {
            call->data.push_back(data);  // The arrays stay valid even if a command replaces it.
        }
        call->last_error.clear();
        return FEM_OK;
    } catch (const std::exception& e) {
        call->last_error = e.what();
    } catch (...) {
        call->last_error = "unknown error";
    }
    return FEM_ERROR;
}

const FemCommandApi command_api = {
    static_cast<std::uint32_t>(sizeof(FemCommandApi)),
    call_command_id,
    call_arguments,
    call_execute,
    call_last_error,
    call_set_result,
    call_set_error,
    call_create_object,
    call_report_problem,
    call_read_model,
    call_object_data,
};

}  // namespace

/// An activated native module: its handlers and the host API it was given.
class NativeInstance final : public ExtensionInstance {
public:
    NativeInstance(const ExtensionInfo& extension, ExtensionHost& host,
                   std::shared_ptr<SharedLibrary> library, const FemExtension& abi,
                   const std::string& engine)
        : extension_(extension),
          host_(host),
          library_(std::move(library)),
          abi_(abi),
          id_(extension.id()),
          path_(display(extension.folder)),
          engine_(engine) {}
    NativeInstance(const NativeInstance&) = delete;
    NativeInstance& operator=(const NativeInstance&) = delete;
    ~NativeInstance() override { deactivate(); }

    /// Calls `activate` of the module; throws ExtensionError(activation_failed).
    void activate();
    command::CommandResult execute(command::CommandContext& context) override;
    void deactivate() noexcept override;

    // For the host API.
    FemStatus register_command(const char* id, FemCommandHandler handler, void* user_data);
    FemStatus set_context_key(const char* name, const char* value);
    void set_error(const char* message) { error_ = message != nullptr ? message : ""; }

private:
    struct Handler {
        FemCommandHandler function;
        void* user_data;
    };

    const ExtensionInfo& extension_;
    ExtensionHost& host_;
    std::shared_ptr<SharedLibrary> library_;
    const FemExtension& abi_;
    std::string id_;
    std::string path_;
    std::string engine_;
    FemHost fem_host_;
    FemHostApi api_{};
    std::map<std::string, Handler, std::less<>> handlers_;
    void* state_ = nullptr;
    std::string error_;  // Last error the module or the host API reported.
    bool active_ = false;
};

namespace {

// --- FemHostApi ----------------------------------------------------------------------------------

FemStatus host_register_command(FemHost* host, const char* command_id, FemCommandHandler handler,
                                void* user_data) noexcept {
    try {
        return host->instance->register_command(command_id, handler, user_data);
    } catch (...) {
        return FEM_ERROR;
    }
}

FemStatus host_set_context_key(FemHost* host, const char* name, const char* value) noexcept {
    try {
        return host->instance->set_context_key(name, value);
    } catch (...) {
        return FEM_ERROR;
    }
}

void host_set_error(FemHost* host, const char* message) noexcept {
    try {
        host->instance->set_error(message);
    } catch (...) {
    }
}

}  // namespace

void NativeInstance::activate() {
    fem_host_.instance = this;
    api_ = FemHostApi{
        static_cast<std::uint32_t>(sizeof(FemHostApi)),
        FEM_EXTENSION_ABI_VERSION,
        &fem_host_,
        id_.c_str(),
        path_.c_str(),
        engine_.c_str(),
        &command_api,
        host_register_command,
        host_set_context_key,
        host_set_error,
    };
    if (abi_.activate == nullptr) {
        fail_activation("the module has no activate function");
    }
    void* state = nullptr;
    if (abi_.activate(&api_, &state) != FEM_OK) {
        handlers_.clear();
        fail_activation(error_.empty() ? "activate() of the module reported an error" : error_);
    }
    state_ = state;
    active_ = true;
}

command::CommandResult NativeInstance::execute(command::CommandContext& context) {
    const std::string& command = context.spec().id;
    const auto handler = handlers_.find(command);
    if (handler == handlers_.end()) {
        throw ExtensionError(ExtensionErrc::command_failed,
                             "module '" + id_ + "' gave no code for command '" + command + "'");
    }
    FemCommandCall call;
    call.context = &context;
    call.arguments = to_json(command::Value(context.args().values())).dump();
    if (handler->second.function(&call, handler->second.user_data) != FEM_OK) {
        const std::string reason = !call.error.empty()        ? call.error
                                   : !call.last_error.empty() ? call.last_error
                                                              : "the module reported an error";
        throw ExtensionError(ExtensionErrc::command_failed,
                             "command '" + command + "' failed: " + reason);
    }
    if (!call.result) {
        return {};
    }
    try {
        return command::CommandResult(from_json(nlohmann::json::parse(*call.result)));
    } catch (const nlohmann::json::exception& e) {
        throw ExtensionError(
            ExtensionErrc::command_failed,
            "command '" + command + "' returned a result that is not JSON: " + e.what());
    }
}

void NativeInstance::deactivate() noexcept {
    if (active_ && abi_.deactivate != nullptr) {
        abi_.deactivate(state_);
    }
    active_ = false;
    state_ = nullptr;
    handlers_.clear();
}

FemStatus NativeInstance::register_command(const char* id, FemCommandHandler handler,
                                           void* user_data) {
    if (id == nullptr || handler == nullptr) {
        error_ = "register_command needs a command id and a handler";
        return FEM_ERROR;
    }
    const std::vector<CommandContribution>& commands = extension_.manifest->contributes.commands;
    const bool declared = std::any_of(commands.begin(), commands.end(),
                                      [id](const CommandContribution& c) { return c.id == id; });
    if (!declared) {
        error_ =
            "command '" + std::string(id) + "' is not declared in the manifest of '" + id_ + "'";
        return FEM_ERROR;
    }
    handlers_.insert_or_assign(std::string(id), Handler{handler, user_data});
    return FEM_OK;
}

FemStatus NativeInstance::set_context_key(const char* name, const char* value) {
    try {
        if (name == nullptr) {
            throw ExtensionError(ExtensionErrc::invalid_state, "set_context_key needs a key name");
        }
        host_.set_context_key(
            name, to_context_value(nlohmann::json::parse(value != nullptr ? value : "null")));
        return FEM_OK;
    } catch (const std::exception& e) {
        error_ = e.what();
        return FEM_ERROR;
    }
}

// --- NativeRuntime -------------------------------------------------------------------------------

std::unique_ptr<ExtensionInstance> NativeRuntime::activate(const ExtensionInfo& extension,
                                                           ExtensionHost& host) {
    const std::string name = extension.manifest->entry + std::string(SharedLibrary::suffix());
    const std::filesystem::path file =
        extension.folder / std::filesystem::path(std::u8string(name.begin(), name.end()));
    const std::shared_ptr<SharedLibrary> library = load_library(file);
    const auto entry = library->function<FemExtensionEntry>(FEM_EXTENSION_ENTRY_NAME);
    if (entry == nullptr) {
        fail_activation("'" + display(file) + "' does not export " FEM_EXTENSION_ENTRY_NAME);
    }
    const FemExtension* abi = entry();
    if (abi == nullptr) {
        fail_activation(FEM_EXTENSION_ENTRY_NAME "() of the module returned NULL");
    }
    if (abi->struct_size < sizeof(FemExtension)) {
        fail_activation("FemExtension of the module has " + std::to_string(abi->struct_size) +
                        " bytes, fewer than " + std::to_string(sizeof(FemExtension)));
    }
    if (abi->abi_version != FEM_EXTENSION_ABI_VERSION) {
        fail_activation("the module is built for ABI version " + std::to_string(abi->abi_version) +
                        ", this platform supports version " +
                        std::to_string(FEM_EXTENSION_ABI_VERSION));
    }
    auto instance = std::make_unique<NativeInstance>(extension, host, library, *abi, engine_);
    instance->activate();
    return instance;
}

}  // namespace fem::extension::detail
