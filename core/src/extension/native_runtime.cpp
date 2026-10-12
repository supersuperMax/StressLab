#include "native_runtime.h"

#include "fem/core/command/command.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/extension_abi.h"

#include "files.h"
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
};

namespace fem::extension::detail {

namespace {

[[noreturn]] void fail_activation(const std::string& message) {
    throw ExtensionError(ExtensionErrc::activation_failed, message);
}

nlohmann::json to_json(const command::Value& value) {
    using Kind = command::Value::Kind;
    switch (value.kind()) {
        case Kind::none:
            return nullptr;
        case Kind::boolean:
            return value.as_boolean();
        case Kind::integer:
            return value.as_integer();
        case Kind::number:
            return value.as_number();
        case Kind::string:
            return value.as_string();
        case Kind::path:
            return display(value.as_path());
        case Kind::list: {
            nlohmann::json list = nlohmann::json::array();
            for (const command::Value& item : value.as_list()) {
                list.push_back(to_json(item));
            }
            return list;
        }
        case Kind::dict: {
            nlohmann::json dict = nlohmann::json::object();
            for (const auto& [key, item] : value.as_dict()) {
                dict[key] = to_json(item);
            }
            return dict;
        }
    }
    return nullptr;
}

command::Value from_json(const nlohmann::json& json) {
    switch (json.type()) {
        case nlohmann::json::value_t::null:
            return {};
        case nlohmann::json::value_t::boolean:
            return json.get<bool>();
        case nlohmann::json::value_t::number_integer:
            return json.get<std::int64_t>();
        case nlohmann::json::value_t::number_unsigned: {
            const auto value = json.get<std::uint64_t>();
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw ExtensionError(ExtensionErrc::command_failed,
                                     "the integer " + std::to_string(value) + " is too large");
            }
            return static_cast<std::int64_t>(value);
        }
        case nlohmann::json::value_t::number_float:
            return json.get<double>();
        case nlohmann::json::value_t::string:
            return json.get<std::string>();
        case nlohmann::json::value_t::array: {
            command::Value::List list;
            for (const nlohmann::json& item : json) {
                list.push_back(from_json(item));
            }
            return list;
        }
        case nlohmann::json::value_t::object: {
            command::Value::Dict dict;
            for (const auto& [key, item] : json.items()) {
                dict.emplace(key, from_json(item));
            }
            return dict;
        }
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            break;
    }
    throw ExtensionError(ExtensionErrc::command_failed, "a value that JSON text cannot hold");
}

context::ContextValue to_context_value(const nlohmann::json& json) {
    switch (json.type()) {
        case nlohmann::json::value_t::null:
            return {};
        case nlohmann::json::value_t::boolean:
            return json.get<bool>();
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        case nlohmann::json::value_t::number_float:
            return json.get<double>();
        case nlohmann::json::value_t::string:
            return json.get<std::string>();
        case nlohmann::json::value_t::array: {
            context::ContextValue::List list;
            for (const nlohmann::json& item : json) {
                if (!item.is_string()) {
                    break;
                }
                list.push_back(item.get<std::string>());
            }
            if (list.size() == json.size()) {
                return list;
            }
            break;
        }
        default:
            break;
    }
    throw ExtensionError(ExtensionErrc::invalid_state,
                         "a context key holds true, false, a number, text, an array of text or "
                         "null, not " +
                             json.dump());
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

const FemCommandApi command_api = {
    static_cast<std::uint32_t>(sizeof(FemCommandApi)),
    call_command_id,
    call_arguments,
    call_execute,
    call_last_error,
    call_set_result,
    call_set_error,
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
