#pragma once

#include "fem/core/form/form.h"
#include "fem/core/signal.h"
#include "fem/shell/form_widget.h"
#include "fem/shell/workbench.h"

#include <QWidget>
#include <memory>
#include <string_view>

namespace fem::shell {

/// The zone "Settings": the settings of the core and of the enabled modules
/// (docs/architecture.md §4.2 `settings`) in the form the properties of objects have, one section
/// per module. A field changes the setting at the level of the user and the settings of the user
/// are saved at once (`Workbench::save_settings`); an emptied field goes back to the default. A
/// value set in the project, which wins, is noted under the field. Settings are not the model: no
/// Command Bus, undo or journal (docs/adr/0008-settings-logging-l10n.md).
///
/// The form follows the settings when they change elsewhere, and is built again when modules are
/// enabled or disabled and when the language changes. Object name of the form: "settingsForm".
class SettingsPanel : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPanel(Workbench& workbench, QWidget* parent = nullptr);
    ~SettingsPanel() override;
    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;
    SettingsPanel(SettingsPanel&&) = delete;
    SettingsPanel& operator=(SettingsPanel&&) = delete;

    [[nodiscard]] FormWidget& form() const noexcept { return *form_; }
    /// The form as it is drawn: sections titled with the names of the modules.
    [[nodiscard]] const form::FormSpec& spec() const noexcept { return spec_; }
    /// Scrolls to the section of an owner (a module, or the core) and puts the focus on its first
    /// field; false if it has no section (task 1.14: "Configure" in the module manager).
    bool show_section(std::string_view owner);

private:
    class Source;

    void rebuild();

    Workbench& workbench_;
    form::FormSpec spec_;
    std::unique_ptr<Source> source_;
    FormWidget* form_ = nullptr;
    bool writing_ = false;
    Subscription changed_;
    Subscription extension_events_;
    Subscription language_;
};

}  // namespace fem::shell
