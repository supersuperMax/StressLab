#include "fem/shell/settings_panel.h"

#include "fem/core/log/log.h"
#include "fem/core/model/object.h"
#include "fem/core/settings/error.h"
#include "fem/shell/strings.h"

#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>
#include <vector>

namespace fem::shell {

/// The settings of the user, as the form reads and writes them.
class SettingsPanel::Source final : public FormSource {
public:
    explicit Source(Workbench& workbench, bool& writing)
        : workbench_(workbench), writing_(writing) {}

    [[nodiscard]] nlohmann::json value(const form::Field& field) const override {
        const settings::Settings& settings = workbench_.settings;
        if (settings.schema(field.key) == nullptr) {
            return nullptr;
        }
        // What is in effect: a field shows the default as its value, not as a hint.
        return settings.value(field.key);
    }

    QString write(const form::Field& field, const nlohmann::json& value) override {
        settings::Settings& settings = workbench_.settings;
        writing_ = true;
        try {
            if (value.is_null()) {
                (void)settings.reset(settings::Level::user, field.key);
            } else {
                settings.set(settings::Level::user, field.key, value);
            }
        } catch (const settings::SettingsError& error) {
            writing_ = false;
            return to_qstring(error.what());
        }
        writing_ = false;
        if (workbench_.save_settings) {
            try {
                workbench_.save_settings();
            } catch (const std::exception& error) {
                log::logger("shell")->warn("the settings of the user are not saved: {}",
                                           error.what());
                return text(workbench_.localization, "settings.notSaved",
                            {{"message", std::string(error.what())}});
            }
        }
        return {};
    }

    [[nodiscard]] std::string owner(const form::Field& field) const override {
        const settings::Schema* schema = workbench_.settings.schema(field.key);
        return schema != nullptr ? schema->owner : std::string(model::core_owner);
    }

    [[nodiscard]] QString note(const form::Field& field) const override {
        const settings::SettingState state = workbench_.settings.state(field.key);
        if (!state.project) {
            return {};
        }
        return text(workbench_.localization, "settings.projectValue",
                    {{"value", state.project->dump(-1, ' ', false,
                                                   nlohmann::json::error_handler_t::replace)}});
    }

private:
    Workbench& workbench_;
    bool& writing_;
};

SettingsPanel::SettingsPanel(Workbench& workbench, QWidget* parent)
    : QWidget(parent), workbench_(workbench) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget(scroll);
    auto* holder_layout = new QVBoxLayout(holder);
    holder_layout->setContentsMargins(10, 4, 10, 10);
    form_ = new FormWidget(workbench_.localization, holder);
    form_->setObjectName(QStringLiteral("settingsForm"));
    holder_layout->addWidget(form_);
    holder_layout->addStretch(1);
    scroll->setWidget(holder);
    layout->addWidget(scroll);

    source_ = std::make_unique<Source>(workbench_, writing_);
    rebuild();

    changed_ = workbench_.settings.on_changed([this](std::string_view) {
        if (!writing_) {
            form_->refresh();
        }
    });
    extension_events_ =
        workbench_.extensions.on_event([this](const extension::ExtensionEvent& event) {
            using Kind = extension::ExtensionEvent::Kind;
            if (event.kind == Kind::enabled || event.kind == Kind::deactivated) {
                rebuild();
            }
        });
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { rebuild(); });
}

SettingsPanel::~SettingsPanel() {
    form_->set_form(nullptr, nullptr);
}

void SettingsPanel::rebuild() {
    // The core first, then the modules by id; `settings_form` sorts the settings of each by key.
    std::vector<const settings::Schema*> ordered = workbench_.settings.schemas();
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const settings::Schema* a, const settings::Schema* b) {
                         const bool a_core = a->owner == model::core_owner;
                         const bool b_core = b->owner == model::core_owner;
                         return a_core != b_core ? a_core : a->owner < b->owner;
                     });
    spec_ = form::settings_form(ordered);
    for (form::Field& section : spec_.fields) {
        if (section.key == model::core_owner) {
            section.title = text(workbench_.localization, "settings.general").toStdString();
        } else if (const extension::ExtensionInfo* info = workbench_.extensions.find(section.key);
                   info != nullptr && info->manifest) {
            section.title = workbench_.localization.manifest_text(info->id(), info->manifest->name);
        }
    }
    form_->set_form(&spec_, source_.get(), "settings");
}

}  // namespace fem::shell
