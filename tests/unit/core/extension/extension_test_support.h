#pragma once

#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/extension/runtime.h"
#include "fem/core/extension/semver.h"
#include "fem/core/model/document.h"
#include "fem/core/signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::extension::testing {

/// The platform version the tests run as: the fixtures say engine = "^1.0".
inline const Version engine{1, 4, 0};

/// tests/extensions: manifests and the native test module.
inline std::filesystem::path fixtures() {
    return std::filesystem::path(FEM_TEST_EXTENSIONS_DIR);
}

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// A manifest written in a test, parsed for `engine`.
inline Manifest manifest(std::string_view text) {
    return parse_manifest(text, engine);
}

/// A document with its Command Bus, context keys and an Extension Manager on them, destroyed in
/// the right order.
struct Workspace {
    model::Document document;
    command::CommandBus bus{document};
    context::ContextKeys keys;
    ExtensionManager manager{document, bus, keys, {.engine = engine}};

    /// Installs a module written in a test, from a folder that does not need to exist.
    const ExtensionInfo& install(std::string_view text,
                                 ExtensionSource source = ExtensionSource::builtin) {
        Manifest parsed = manifest(text);
        const std::filesystem::path folder =
            std::filesystem::temp_directory_path() / "stresslab-test-modules" / parsed.id;
        return manager.install(std::move(parsed), folder, source);
    }
};

/// Events of a manager as "extension.enabled test.base".
class EventLog {
public:
    explicit EventLog(ExtensionManager& manager)
        : subscription_(manager.on_event([this](const ExtensionEvent& event) {
              lines_.push_back(std::string(to_string(event.kind)) + " " + event.id);
              events_.push_back(event);
          })) {}

    /// The events since the last call.
    std::vector<std::string> take() { return std::exchange(lines_, {}); }
    [[nodiscard]] const std::vector<ExtensionEvent>& events() const { return events_; }

private:
    std::vector<std::string> lines_;
    std::vector<ExtensionEvent> events_;
    Subscription subscription_;
};

/// A runtime standing in for the extension host of python modules (task 1.9): it logs what
/// happens and runs handlers the test gives.
class FakeRuntime final : public ExtensionRuntime {
public:
    /// Code of commands, by command id. A command without one returns nothing.
    std::map<std::string, command::CommandHandler, std::less<>> handlers;
    /// Modules whose activation throws.
    std::set<std::string, std::less<>> broken;
    /// Called while a module activates, e.g. to set its context keys.
    std::function<void(ExtensionHost&)> on_activate;
    /// "activate test.base", "execute base.hello", "deactivate test.base".
    std::vector<std::string> log;

    [[nodiscard]] std::unique_ptr<ExtensionInstance> activate(const ExtensionInfo& extension,
                                                              ExtensionHost& host) override {
        log.push_back("activate " + extension.id());
        if (broken.contains(extension.id())) {
            throw std::runtime_error("broken on purpose");
        }
        if (on_activate) {
            on_activate(host);
        }
        return std::make_unique<Instance>(*this, extension.id());
    }

    /// The log since the last call.
    std::vector<std::string> take() { return std::exchange(log, {}); }

private:
    class Instance final : public ExtensionInstance {
    public:
        Instance(FakeRuntime& runtime, std::string id) : runtime_(runtime), id_(std::move(id)) {}

        command::CommandResult execute(command::CommandContext& context) override {
            runtime_.log.push_back("execute " + context.spec().id);
            const auto handler = runtime_.handlers.find(context.spec().id);
            return handler == runtime_.handlers.end() ? command::CommandResult{}
                                                      : handler->second(context);
        }
        void deactivate() noexcept override { runtime_.log.push_back("deactivate " + id_); }

    private:
        FakeRuntime& runtime_;
        std::string id_;
    };
};

}  // namespace fem::extension::testing

/// Checks that `statement` throws ExtensionError with the given code.
#define EXPECT_EXTENSION_ERROR(statement, expected_code)                 \
    do {                                                                 \
        try {                                                            \
            statement;                                                   \
            ADD_FAILURE() << "Expected ExtensionError "                  \
                          << ::fem::extension::to_string(expected_code); \
        } catch (const ::fem::extension::ExtensionError& error) {        \
            EXPECT_EQ(::fem::extension::to_string(error.code()),         \
                      ::fem::extension::to_string(expected_code))        \
                << error.what();                                         \
        }                                                                \
    } while (false)
