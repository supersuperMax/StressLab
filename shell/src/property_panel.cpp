#include "fem/shell/property_panel.h"

#include "fem/core/command/model_commands.h"
#include "fem/core/log/log.h"
#include "fem/core/model/object.h"
#include "fem/shell/model_tree.h"
#include "fem/shell/strings.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <exception>

namespace fem::shell {

namespace {

/// A value of a form field as an argument of a command. Forms write scalars only.
command::Value to_value(const nlohmann::json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return {};
}

}  // namespace

/// The properties of one object: read from the document, written with `model.set_property`.
class PropertyPanel::Source final : public FormSource {
public:
    Source(Workbench& workbench, model::ObjectId id) : workbench_(workbench), id_(id) {}

    [[nodiscard]] nlohmann::json value(const form::Field& field) const override {
        const model::Object* object = workbench_.bus.document().find(id_);
        if (object == nullptr) {
            return nullptr;
        }
        const nlohmann::json& properties = object->properties();
        try {
            const nlohmann::json::json_pointer pointer("/" + field.pointer);
            return properties.contains(pointer) ? properties.at(pointer) : nlohmann::json();
        } catch (const nlohmann::json::exception&) {
            return nullptr;  // A name on the way holds no object.
        }
    }

    QString write(const form::Field& field, const nlohmann::json& value) override {
        try {
            (void)workbench_.bus.execute(command::model_commands::set_property,
                                         {{"object", command::Value(id_)},
                                          {"key", field.pointer},
                                          {"value", to_value(value)}});
            return {};
        } catch (const std::exception& error) {
            log::logger("shell")->warn("setting '{}' of {} failed: {}", field.pointer,
                                       model::to_string(id_), error.what());
            return to_qstring(error.what());
        }
    }

    [[nodiscard]] std::string owner(const form::Field& /*field*/) const override {
        const model::Object* object = workbench_.bus.document().find(id_);
        return object != nullptr ? object->owner() : std::string();
    }

    [[nodiscard]] model::UnitSystem units() const override {
        return workbench_.bus.document().units();
    }

private:
    Workbench& workbench_;
    model::ObjectId id_;
};

PropertyPanel::PropertyPanel(Workbench& workbench, QWidget* parent)
    : QWidget(parent), workbench_(workbench), forms_(workbench.extensions.registry()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(10, 8, 10, 6);
    header_layout->setSpacing(2);
    title_ = new QLabel(header);
    title_->setObjectName(QStringLiteral("propertiesTitle"));
    title_->setTextFormat(Qt::PlainText);
    QFont font = title_->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.15);
    title_->setFont(font);
    type_ = new QLabel(header);
    type_->setObjectName(QStringLiteral("propertiesType"));
    type_->setTextFormat(Qt::PlainText);
    type_->setForegroundRole(QPalette::PlaceholderText);
    header_layout->addWidget(title_);
    header_layout->addWidget(type_);
    layout->addWidget(header);

    empty_ = new QLabel(this);
    empty_->setObjectName(QStringLiteral("propertiesEmpty"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setMargin(12);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(empty_, 1);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget(scroll_);
    auto* holder_layout = new QVBoxLayout(holder);
    holder_layout->setContentsMargins(10, 4, 10, 10);
    form_ = new FormWidget(workbench_.localization, holder);
    form_->setObjectName(QStringLiteral("propertiesForm"));
    holder_layout->addWidget(form_);
    holder_layout->addStretch(1);
    scroll_->setWidget(holder);
    layout->addWidget(scroll_, 1);

    model_changed_ = workbench_.bus.on_model_changed([this](const command::ModelChange& change) {
        if (!object_.is_valid()) {
            return;
        }
        if (!workbench_.bus.document().contains(object_) ||
            workbench_.bus.document().is_opaque(object_)) {
            show_object({});
            return;
        }
        if (change.summary.units) {
            form_->rebuild();  // Units of quantities change with the unit system.
        } else {
            form_->refresh();
        }
        update_heading();
    });
    extension_events_ =
        workbench_.extensions.on_event([this](const extension::ExtensionEvent& event) {
            using Kind = extension::ExtensionEvent::Kind;
            if (event.kind == Kind::enabled || event.kind == Kind::deactivated) {
                forms_ = form::FormIndex(workbench_.extensions.registry());
                show_object(object_);
            }
        });
    language_ = workbench_.localization.on_language_changed([this](const std::string&) {
        update_heading();
        form_->rebuild();
    });
    show_object({});
}

PropertyPanel::~PropertyPanel() {
    form_->set_form(nullptr, nullptr);
}

void PropertyPanel::show_object(model::ObjectId id) {
    const model::Document& document = workbench_.bus.document();
    object_ = document.contains(id) && !document.is_opaque(id) ? id : model::ObjectId();
    rebuild();
}

void PropertyPanel::rebuild() {
    const model::Object* object = workbench_.bus.document().find(object_);
    const form::PropertyForm* found =
        object != nullptr ? forms_.find(object->owner(), object->type()) : nullptr;
    // The old source stays until the form lets go of it.
    std::unique_ptr<Source> source =
        found != nullptr ? std::make_unique<Source>(workbench_, object_) : nullptr;
    form_->set_form(found != nullptr ? &found->form : nullptr, source.get(),
                    object != nullptr ? object->owner() + "\n" + object->type() : std::string());
    source_ = std::move(source);
    update_heading();
}

void PropertyPanel::update_heading() {
    const model::Object* object = workbench_.bus.document().find(object_);
    if (object == nullptr) {
        title_->clear();
        type_->clear();
        title_->hide();
        type_->hide();
        empty_->setText(text("properties.none"));
        empty_->show();
        scroll_->hide();
        return;
    }
    title_->setText(object_title(workbench_, *object));
    type_->setText(type_title(workbench_, object->owner(), object->type()));
    title_->show();
    type_->show();
    const bool has_form = !form_->empty();
    empty_->setText(has_form ? QString() : text("properties.noForm"));
    empty_->setVisible(!has_form);
    scroll_->setVisible(has_form);
}

QString PropertyPanel::title() const {
    return title_->text();
}

QString PropertyPanel::empty_text() const {
    return empty_->isVisibleTo(this) ? empty_->text() : QString();
}

QString PropertyPanel::text(std::string_view key) const {
    return shell::text(workbench_.localization, key);
}

}  // namespace fem::shell
