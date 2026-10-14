#pragma once

#include "fem/core/extension/manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/signal.h"
#include "fem/shell/workbench.h"

#include <nlohmann/json.hpp>

#include <QString>
#include <QStringList>
#include <QWidget>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMessageBox;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace fem::shell {

/// What the window lends the module manager.
struct ExtensionsHooks {
    /// The layout of the window as JSON, kept in a profile.
    std::function<nlohmann::json()> layout = {};
    /// Puts the window into the layout of a profile; false if it cannot be read.
    std::function<bool(const nlohmann::json&)> restore_layout = {};
    /// Opens the settings at the section of a module.
    std::function<void(const std::string&)> show_settings = {};
    /// Where messages for the user go besides the panel (the prompt line of the window).
    std::function<void(const QString&)> report = {};
};

/// A point of docs/interface-design.md §6 where a module adds something, with what it adds there:
/// "Stages: Optimization", "Commands: Run, Stop".
struct ContributionLine {
    QString point;
    QStringList items;
};

/// What a module adds to the interface, by point, in the order of the table of
/// docs/interface-design.md §6; points it adds nothing to are left out.
[[nodiscard]] std::vector<ContributionLine> contribution_lines(
    const extension::ExtensionInfo& extension, const l10n::Localization& localization);

/// The module manager (task 1.14, docs/interface-design.md §6 "Module manager",
/// docs/architecture.md §4.5): the side panel of the activity "Modules".
///
/// - A search over names, ids and descriptions, and the tabs "Installed", "Catalog" (empty until
///   there is a catalog) and "Updates" (modules installed from a folder whose folder has another
///   version now).
/// - A row per installed module with its switch, its version, the mark "Local" for modules
///   installed from a folder, and why it cannot run or failed to start; a click opens its card.
/// - The card: description, version, author, where it comes from, what it adds to the interface,
///   the modules it requires and their state; "Enable"/"Disable", "Configure" (its section of the
///   settings) and "Uninstall" (not for modules shipped with the application).
/// - Disabling a module whose objects are in the model (or that other enabled modules need) asks
///   first: the objects are hidden but kept in the file, and come back when it is enabled again.
/// - "Install from folder…": a module of the user, not verified, after a warning.
/// - Profiles: sets of enabled modules with the layout of the window; apply, save the current
///   state as one, delete, import and export as files.
///
/// Everything it changes is kept between sessions through `Workbench::save_modules`. The lists are
/// rebuilt after the Extension Manager reports a change, in the event loop (`rebuild` at once).
///
/// Object names, for tests: "extensions.search", "extensions.tabs", "extensions.installed",
/// "extensions.updates", "extensions.catalog", "extensions.status", "extensions.installFolder",
/// switches "toggle:<id>", update buttons "update:<id>", the card "extensions.card" with
/// "card.back", "card.toggle", "card.configure", "card.uninstall"; "profiles.box",
/// "profiles.apply", "profiles.more" and its actions "profiles.saveAs", "profiles.delete",
/// "profiles.import", "profiles.export"; dialogs "disableModuleDialog", "installModuleDialog",
/// "uninstallModuleDialog", "applyProfileDialog", "profileNameDialog" with the buttons "accept"
/// and "cancel".
class ExtensionsPanel : public QWidget {
    Q_OBJECT

public:
    enum class Tab : int { installed = 0, catalog = 1, updates = 2 };

    explicit ExtensionsPanel(Workbench& workbench, QWidget* parent = nullptr);
    ~ExtensionsPanel() override;
    ExtensionsPanel(const ExtensionsPanel&) = delete;
    ExtensionsPanel& operator=(const ExtensionsPanel&) = delete;
    ExtensionsPanel(ExtensionsPanel&&) = delete;
    ExtensionsPanel& operator=(ExtensionsPanel&&) = delete;

    void set_hooks(ExtensionsHooks hooks) { hooks_ = std::move(hooks); }

    /// Builds the lists, the card and the profiles again from the Extension Manager.
    void rebuild();

    // --- The lists -------------------------------------------------------------------------------

    [[nodiscard]] QLineEdit* search() const noexcept { return search_; }
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    void show_tab(Tab tab);
    /// Ids of the modules the "Installed" tab lists now (after the search), in order.
    [[nodiscard]] std::vector<std::string> listed() const;
    /// The switch of a module in the "Installed" list; nullptr if it is not listed.
    [[nodiscard]] QCheckBox* toggle(std::string_view id) const;
    /// The text of the row of a module, as plain text; empty if it is not listed.
    [[nodiscard]] QString row_text(std::string_view id) const;
    /// Ids the "Updates" tab offers.
    [[nodiscard]] std::vector<std::string> offered_updates() const;
    /// The message at the top of the panel: what failed, what a profile could not do.
    [[nodiscard]] QString status() const;

    // --- The card --------------------------------------------------------------------------------

    void show_card(const std::string& id);
    void show_list();
    /// The id of the module whose card is shown; empty: the lists are shown.
    [[nodiscard]] const std::string& card() const noexcept { return card_; }
    /// The card as plain text, a line per label.
    [[nodiscard]] QString card_text() const;

    // --- What the buttons do ---------------------------------------------------------------------

    /// Enables or disables a module. Disabling asks first ("disableModuleDialog") if objects of
    /// the model would be hidden or other modules disabled with it.
    void set_enabled(const std::string& id, bool enabled);
    /// Asks for a folder, then warns ("installModuleDialog") and installs and enables its module.
    void install_from_folder();
    /// Warns, then installs and enables the module of a folder; returns the warning.
    QMessageBox* ask_install(const std::filesystem::path& folder);
    /// Asks ("uninstallModuleDialog"), then uninstalls a module.
    QMessageBox* ask_uninstall(const std::string& id);
    /// Reads the manifest of a module from its folder again (the "Updates" tab).
    void update_module(const std::string& id);

    // --- Profiles --------------------------------------------------------------------------------

    [[nodiscard]] QComboBox* profile_box() const noexcept { return profiles_box_; }
    /// Applies a profile of the library, asking first ("applyProfileDialog") if objects of the
    /// model would be hidden.
    void apply_profile(const std::string& name);
    /// Saves the enabled modules and the layout of the window as a profile of this name.
    void save_profile(const std::string& name);
    /// Asks for a name ("profileNameDialog") and saves the profile.
    QWidget* ask_profile_name();
    void delete_profile(const std::string& name);
    /// Asks for a profile file and adds it to the library, replacing one of the same name.
    void import_profile();
    /// Asks for a file and writes a profile of the library there.
    void export_profile(const std::string& name);

Q_SIGNALS:
    /// The panel was built again.
    void rebuilt();

private:
    void build_list_page();
    void build_card_page();
    void build_profiles(QVBoxLayout* layout);
    void rebuild_installed();
    void rebuild_updates();
    void rebuild_card();
    void rebuild_profiles();
    /// Rebuilds once the event loop gets to it: never inside a signal of a widget it deletes.
    void schedule_rebuild();
    void disable_now(const std::string& id);
    void install_now(const std::filesystem::path& folder);
    void apply_now(const extension::Profile& profile);
    void changed_modules();
    void set_status(const QString& message);
    void retranslate();
    /// A question with "accept" and "cancel" that runs `accepted` if accepted; not modal to the
    /// window, it deletes itself when closed.
    QMessageBox* confirm(const QString& name, const QString& text, const QString& accept,
                         std::function<void()> accepted);
    [[nodiscard]] QString module_name(const extension::ExtensionInfo& info) const;
    [[nodiscard]] QString objects_warning(const std::vector<std::string>& modules) const;
    [[nodiscard]] QString tr_text(std::string_view key, const l10n::Args& args = {}) const;
    [[nodiscard]] std::string selected_profile() const;

    Workbench& workbench_;
    ExtensionsHooks hooks_;
    QLabel* status_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    // The lists.
    QLineEdit* search_ = nullptr;
    QPushButton* install_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QListWidget* installed_ = nullptr;
    QLabel* catalog_ = nullptr;
    QListWidget* updates_ = nullptr;
    QLabel* no_updates_ = nullptr;
    QLabel* profiles_label_ = nullptr;
    QComboBox* profiles_box_ = nullptr;
    QPushButton* apply_ = nullptr;
    QToolButton* more_ = nullptr;
    // The card.
    QWidget* card_page_ = nullptr;
    QPushButton* back_ = nullptr;
    QVBoxLayout* card_layout_ = nullptr;
    QWidget* card_content_ = nullptr;
    std::string card_;

    std::vector<std::string> listed_;
    std::vector<std::string> updates_offered_;
    bool rebuild_pending_ = false;
    Subscription events_;
    Subscription language_;
};

}  // namespace fem::shell
