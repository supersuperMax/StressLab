#pragma once

// What tests of the window need: the parts of the application it works with (a document, its
// Command Bus and context keys, the localization with the strings of the shell, an Extension
// Manager whose python modules run in a fake runtime) and modules written in the tests.

#include "fem/core/bindings.h"
#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/extension/runtime.h"
#include "fem/core/extension/semver.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/document.h"
#include "fem/core/settings/settings.h"
#include "fem/core/signal.h"
#include "fem/shell/strings.h"
#include "fem/shell/workbench.h"
#include "fem/shell/workspaces.h"

#include <QAction>
#include <QList>
#include <QMenu>
#include <QString>
#include <QWidget>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fem::shell::testing {

/// The platform version the tests run as: modules of the tests say engine = "^1.0".
inline const extension::Version engine{1, 4, 0};

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

inline QString q(std::string_view text) {
    return to_qstring(text);
}

/// A runtime standing in for the extension host: commands of modules run here.
class FakeRuntime final : public extension::ExtensionRuntime {
public:
    /// "execute <command>" for every command run.
    std::vector<std::string> log;
    /// Modules whose activation throws.
    std::set<std::string, std::less<>> broken;
    /// The hosts of the active modules, by module id, to report crashes.
    std::map<std::string, extension::ExtensionHost*, std::less<>> hosts;

    [[nodiscard]] std::unique_ptr<extension::ExtensionInstance> activate(
        const extension::ExtensionInfo& extension, extension::ExtensionHost& host) override {
        if (broken.contains(extension.id())) {
            throw std::runtime_error("broken on purpose");
        }
        hosts[extension.id()] = &host;
        return std::make_unique<Instance>(*this, extension.id());
    }

private:
    class Instance final : public extension::ExtensionInstance {
    public:
        Instance(FakeRuntime& runtime, std::string id) : runtime_(runtime), id_(std::move(id)) {}
        command::CommandResult execute(command::CommandContext& context) override {
            runtime_.log.push_back("execute " + context.spec().id);
            return {};
        }
        void deactivate() noexcept override { runtime_.hosts.erase(id_); }

    private:
        FakeRuntime& runtime_;
        std::string id_;
    };
};

/// The parts of the application a window works with, destroyed in the right order.
struct Application {
    Application() {
        add_shell_catalogs(localization);
        manager.set_runtime(extension::ExtensionKind::python, runtime);
    }

    model::Document document;
    command::CommandBus bus{document};
    context::ContextKeys keys;
    context::CoreKeys core_keys{bus, keys};
    l10n::Localization localization;
    WorkspaceStore workspaces;
    extension::ExtensionManager manager{document, bus, keys, {.engine = engine}};
    std::shared_ptr<FakeRuntime> runtime = std::make_shared<FakeRuntime>();
    Subscription catalogs = bind_catalogs(manager, localization);
    settings::Settings settings;
    Subscription settings_schemas = bind_settings(manager, settings);
    /// How many times the window saved the settings of the user.
    int settings_saved = 0;

    [[nodiscard]] Workbench workbench() {
        return {.localization = localization,
                .workspaces = workspaces,
                .extensions = manager,
                .bus = bus,
                .keys = keys,
                .settings = settings,
                .save_settings = [this] { ++settings_saved; }};
    }

    /// Installs a python module written in a test: "[extension]" with this id, the name "Module
    /// <id>" and the lines of `header` comes first, then `contributes`. Its folder is `folder`, or
    /// a folder in the temporary folder that does not need to exist.
    const extension::ExtensionInfo& install(std::string_view id, std::string_view contributes,
                                            std::optional<std::filesystem::path> folder = {},
                                            std::string_view header = {}) {
        const std::string text = "[extension]\nid = \"" + std::string(id) + "\"\nname = \"Module " +
                                 std::string(id) +
                                 "\"\nversion = \"1.0.0\"\nengine = \"^1.0\"\nkind = \"python\"\n"
                                 "entry = \"module:activate\"\n" +
                                 std::string(header) + std::string(contributes);
        extension::Manifest manifest = extension::parse_manifest(text, engine);
        const std::filesystem::path where = folder.value_or(
            std::filesystem::temp_directory_path() / "stresslab-shell-tests" / std::string(id));
        return manager.install(std::move(manifest), where, extension::ExtensionSource::builtin);
    }
    /// Installs and enables.
    void add(std::string_view id, std::string_view contributes,
             std::optional<std::filesystem::path> folder = {}) {
        (void)install(id, contributes, std::move(folder));
        (void)manager.enable(id);
    }

#ifdef FEM_BUILTIN_EXTENSIONS_DIR
    /// Installs and enables the module fem.demo the application ships (task 1.15), from its folder,
    /// with its manifest as it is but for `engine`, which names the platform version of the tests.
    void add_demo() {
        const std::filesystem::path folder =
            std::filesystem::path(FEM_BUILTIN_EXTENSIONS_DIR) / "fem.demo";
        std::ifstream stream(folder / "stresslab-extension.toml", std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        const std::string shipped = "engine = \"^0.1\"";
        const std::size_t at = text.find(shipped);
        if (at == std::string::npos) {
            throw std::runtime_error("the manifest of fem.demo has another engine");
        }
        text.replace(at, shipped.size(), "engine = \"^1.0\"");
        (void)manager.install(extension::parse_manifest(text, engine), folder,
                              extension::ExtensionSource::builtin);
        (void)manager.enable("fem.demo");
    }
#endif
};

/// The action of a menu or a widget with this object name, or nullptr; submenus are searched too.
inline QAction* find_action(const QWidget* widget, const QString& name) {
    for (QAction* action : widget->actions()) {
        if (action->objectName() == name) {
            return action;
        }
        if (action->menu() != nullptr) {
            if (QAction* inner = find_action(action->menu(), name)) {
                return inner;
            }
        }
    }
    return nullptr;
}

/// The texts of the items of a menu that show, separators as "---".
inline QStringList shown_items(const QMenu* menu) {
    QStringList items;
    for (const QAction* action : menu->actions()) {
        if (action->isVisible()) {
            items.push_back(action->isSeparator() ? QStringLiteral("---") : action->text());
        }
    }
    return items;
}

}  // namespace fem::shell::testing
