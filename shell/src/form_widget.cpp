#include "fem/shell/form_widget.h"

#include "fem/core/model/object.h"
#include "fem/core/model/quantity.h"
#include "fem/shell/brand.h"
#include "fem/shell/strings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace fem::shell {

namespace {

using Json = nlohmann::json;

QString object_name(std::string_view prefix, std::string_view pointer) {
    return QString::fromLatin1(prefix.data(), static_cast<qsizetype>(prefix.size())) +
           to_qstring(pointer);
}

/// The colour a field is tinted with when something is wrong with it: the accent of the brand,
/// faint over the colour of fields of the theme.
QColor tinted(const QColor& base) {
    constexpr double share = 0.22;
    const QColor& accent = brand::coral;
    return QColor::fromRgbF(
        static_cast<float>(base.redF() * (1 - share) + accent.redF() * share),
        static_cast<float>(base.greenF() * (1 - share) + accent.greenF() * share),
        static_cast<float>(base.blueF() * (1 - share) + accent.blueF() * share));
}

}  // namespace

FormWidget::FormWidget(const l10n::Localization& localization, QWidget* parent)
    : QWidget(parent), localization_(localization) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
}

FormWidget::~FormWidget() {
    // The editors outlive the members of this class: QWidget deletes its children after them. A
    // text field that has the focus then reports the end of editing (closing the window takes the
    // focus on Linux): nothing of a child may reach this object any more.
    for (QObject* child : findChildren<QObject*>()) {
        QObject::disconnect(child, nullptr, this, nullptr);
    }
}

void FormWidget::set_form(const form::FormSpec* form, FormSource* source, std::string context) {
    form_ = form;
    source_ = source;
    context_ = std::move(context);
    rebuild();
}

void FormWidget::rebuild() {
    ++generation_;
    entries_.clear();
    advanced_buttons_.clear();
    if (content_ != nullptr) {
        content_->hide();
        content_->deleteLater();
        content_ = nullptr;
    }
    if (form_ == nullptr || source_ == nullptr) {
        return;
    }
    // Entries are referred to by index from the signals of their widgets: no reallocation.
    entries_.reserve(form::all_fields(*form_).size());
    content_ = new QWidget(this);
    auto* layout = new QFormLayout(content_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    build_fields(form_->fields, layout, {});
    static_cast<QVBoxLayout*>(this->layout())->addWidget(content_);
    refresh();
}

void FormWidget::build_fields(const std::vector<form::Field>& fields, QFormLayout* layout,
                              const std::string& group) {
    std::vector<const form::Field*> advanced;
    for (const form::Field& field : fields) {
        if (field.advanced) {
            advanced.push_back(&field);
        } else {
            add_field(field, layout);
        }
    }
    if (advanced.empty()) {
        return;
    }
    auto* button = new QToolButton(layout->parentWidget());
    button->setObjectName(object_name("advanced:", group));
    button->setText(text("form.advanced"));
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* part = new QWidget(layout->parentWidget());
    auto* inner = new QFormLayout(part);
    inner->setContentsMargins(0, 0, 0, 0);
    inner->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const form::Field* field : advanced) {
        add_field(*field, inner);
    }
    const std::string remembered = context_ + "\n" + group;
    const bool open = opened_.contains(remembered);
    button->setChecked(open);
    button->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    part->setVisible(open);
    connect(button, &QToolButton::toggled, this, [this, button, part, remembered](bool on) {
        button->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        part->setVisible(on);
        if (on) {
            opened_.insert(remembered);
        } else {
            opened_.erase(remembered);
        }
    });
    layout->addRow(button);
    layout->addRow(part);
    advanced_buttons_[group] = button;
}

void FormWidget::add_field(const form::Field& field, QFormLayout* layout) {
    QWidget* parent = layout->parentWidget();
    if (field.kind == form::FieldKind::section) {
        auto* heading =
            new QLabel(field_text(field, field.title.empty() ? field.key : field.title), parent);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        heading->setContentsMargins(0, 8, 0, 2);
        layout->addRow(heading);
        build_fields(field.fields, layout, field.pointer);
        return;
    }
    if (field.kind == form::FieldKind::group) {
        auto* box =
            new QGroupBox(field_text(field, field.title.empty() ? field.key : field.title), parent);
        box->setObjectName(object_name("group:", field.pointer));
        auto* inner = new QFormLayout(box);
        inner->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        build_fields(field.fields, inner, field.pointer);
        layout->addRow(box);
        return;
    }
    entries_.push_back({});
    Entry& entry = entries_.back();
    entry.field = &field;
    QString title = field_text(field, field.title.empty() ? field.key : field.title);
    if (field.required) {
        title += QStringLiteral(" *");
    }
    entry.label = new QLabel(title, parent);
    entry.label->setWordWrap(true);
    QWidget* row = make_editor(entry, parent);
    layout->addRow(entry.label, row);
    entry.note = new QLabel(parent);
    entry.note->setForegroundRole(QPalette::PlaceholderText);
    entry.note->setWordWrap(true);
    entry.note->hide();
    layout->addRow(QString(), entry.note);
}

QWidget* FormWidget::make_editor(Entry& entry, QWidget* parent) {
    using form::FieldKind;
    const form::Field& field = *entry.field;
    const std::size_t index = entries_.size() - 1;
    const std::uint64_t generation = generation_;
    const QString name = object_name("field:", field.pointer);

    if (field.kind == FieldKind::boolean) {
        auto* box = new QCheckBox(parent);
        box->setObjectName(name);
        box->setEnabled(!field.read_only);
        connect(box, &QCheckBox::toggled, this, [this, index, generation](bool on) {
            if (Entry* target = entry_at(generation, index)) {
                write(*target, Json(on));
            }
        });
        entry.editor = box;
        return box;
    }
    if (field.kind == FieldKind::choice) {
        auto* list = new QComboBox(parent);
        list->setObjectName(name);
        for (const form::Choice& choice : field.choices) {
            list->addItem(choice.title.empty() ? to_qstring(choice.value.is_string()
                                                                ? choice.value.get<std::string>()
                                                                : choice.value.dump())
                                               : field_text(field, choice.title));
        }
        list->setEnabled(!field.read_only);
        connect(list, &QComboBox::activated, this, [this, index, generation](int item) {
            Entry* target = entry_at(generation, index);
            if (target != nullptr && item >= 0 &&
                static_cast<std::size_t>(item) < target->field->choices.size()) {
                write(*target, target->field->choices[static_cast<std::size_t>(item)].value);
            }
        });
        entry.editor = list;
        return list;
    }

    auto* edit = new QLineEdit(parent);
    edit->setObjectName(name);
    entry.editor = edit;
    if (field.kind == FieldKind::raw || field.read_only) {
        edit->setReadOnly(true);
    } else {
        connect(edit, &QLineEdit::editingFinished, this, [this, index, generation] {
            if (Entry* target = entry_at(generation, index)) {
                edited(*target);
            }
        });
    }
    const bool with_units = field.quantity != nullptr;
    if (!with_units && field.kind != FieldKind::path) {
        return edit;
    }
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit, 1);
    if (field.kind == FieldKind::path) {
        auto* browse = new QToolButton(row);
        browse->setObjectName(object_name("browse:", field.pointer));
        browse->setText(QStringLiteral("…"));
        browse->setEnabled(!field.read_only);
        connect(browse, &QToolButton::clicked, this, [this, index, generation] {
            const Entry* target = entry_at(generation, index);
            if (target == nullptr) {
                return;
            }
            const std::string pointer = target->field->pointer;
            const QString file = QFileDialog::getOpenFileName(
                this,
                field_text(*target->field, target->field->title.empty() ? target->field->key
                                                                        : target->field->title));
            // The dialog ran an event loop: the form may have changed meanwhile.
            if (Entry* still = find(pointer); still != nullptr && !file.isEmpty()) {
                write(*still, Json(file.toStdString()));
            }
        });
        layout->addWidget(browse);
        return row;
    }
    const model::Quantity& quantity = *field.quantity;
    if (field.kind == FieldKind::integer) {
        // Integers keep the unit of the model: another unit would make fractions.
        layout->addWidget(new QLabel(model_unit_label(quantity), row));
        return row;
    }
    auto* units = new QComboBox(row);
    units->setObjectName(object_name("unit:", field.pointer));
    const model::DisplayUnit* own = model::model_unit(quantity, source_->units());
    if (own == nullptr) {
        units->addItem(model_unit_label(quantity), QString());
    }
    for (const model::DisplayUnit& unit : quantity.units) {
        units->addItem(unit_label(unit), to_qstring(unit.id));
    }
    const auto remembered = units_.find(remembered_key(field));
    const QString wanted = remembered != units_.end()
                               ? to_qstring(remembered->second)
                               : (own != nullptr ? to_qstring(own->id) : QString());
    units->setCurrentIndex(std::max(0, units->findData(wanted)));
    connect(units, &QComboBox::currentIndexChanged, this, [this, index, generation] {
        if (Entry* target = entry_at(generation, index)) {
            unit_changed(*target);
        }
    });
    entry.units = units;
    layout->addWidget(units);
    return row;
}

void FormWidget::refresh() {
    for (Entry& entry : entries_) {
        entry.refused.reset();
        show_value(entry);
    }
}

void FormWidget::show_value(Entry& entry) {
    using form::FieldKind;
    const form::Field& field = *entry.field;
    const Json value = source_->value(field);
    const Json& shown = value.is_null() ? field.default_value : value;
    if (auto* box = qobject_cast<QCheckBox*>(entry.editor.data())) {
        const QSignalBlocker block(box);
        box->setChecked(shown.is_boolean() && shown.get<bool>());
    } else if (auto* list = qobject_cast<QComboBox*>(entry.editor.data())) {
        int current = -1;
        int fallback = -1;
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (field.choices[i].value == value) {
                current = static_cast<int>(i);
            }
            if (field.choices[i].value == field.default_value) {
                fallback = static_cast<int>(i);
            }
        }
        const QSignalBlocker block(list);
        list->setPlaceholderText(fallback >= 0 ? list->itemText(fallback) : QString());
        list->setCurrentIndex(current);
    } else if (auto* edit = qobject_cast<QLineEdit*>(entry.editor.data())) {
        QString text;
        QString placeholder;
        const auto format = [&](const Json& json) {
            if (json.is_null()) {
                return QString();
            }
            if (field.kind == FieldKind::number && json.is_number()) {
                return format_number(entry, json.get<double>());
            }
            if (field.kind == FieldKind::integer && json.is_number_integer()) {
                return QString::number(json.get<std::int64_t>());
            }
            if (json.is_string() && field.kind != FieldKind::raw) {
                return to_qstring(json.get<std::string>());
            }
            return to_qstring(json.dump(-1, ' ', false, Json::error_handler_t::replace));
        };
        text = format(value);
        placeholder = format(field.default_value);
        const QSignalBlocker block(edit);
        edit->setText(text);
        edit->setPlaceholderText(placeholder);
        edit->setCursorPosition(0);
        entry.shown = text;
    }
    if (entry.note) {
        const QString note = source_->note(field);
        entry.note->setText(note);
        entry.note->setVisible(!note.isEmpty());
    }
    show_state(entry);
}

void FormWidget::show_state(Entry& entry) {
    const form::Field& field = *entry.field;
    entry.state = form::Severity::ok;
    entry.problem.clear();
    if (entry.refused) {
        entry.state = form::Severity::error;
        entry.problem = *entry.refused;
    } else {
        const Json value = source_->value(field);
        std::optional<form::Problem> problem;
        if (form::is_empty(&value)) {
            if (field.required) {
                problem = form::Problem{.code = form::ProblemCode::required,
                                        .severity = form::Severity::error,
                                        .pointer = field.pointer,
                                        .title = field.title,
                                        .detail = Json::object()};
            }
        } else {
            problem = form::check_value(field, value);
        }
        if (problem) {
            entry.state = problem->severity;
            entry.problem = to_qstring(form::text(*problem, localization_, source_->owner(field)));
        }
    }
    QWidget* editor = entry.editor.data();
    if (editor == nullptr) {
        return;
    }
    const char* state = entry.state == form::Severity::ok        ? ""
                        : entry.state == form::Severity::warning ? "warning"
                                                                 : "error";
    editor->setProperty("problem", QString::fromLatin1(state));
    QPalette palette = this->palette();
    if (entry.state != form::Severity::ok) {
        for (const QPalette::ColorGroup group :
             {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            palette.setColor(group, QPalette::Base, tinted(palette.color(group, QPalette::Base)));
            palette.setColor(group, QPalette::Button,
                             tinted(palette.color(group, QPalette::Button)));
        }
        editor->setPalette(palette);
    } else {
        editor->setPalette(QPalette());
    }
    editor->setToolTip(tooltip(entry));
    if (entry.label) {
        entry.label->setToolTip(editor->toolTip());
    }
}

void FormWidget::edited(Entry& entry) {
    auto* edit = qobject_cast<QLineEdit*>(entry.editor.data());
    if (edit == nullptr) {
        return;
    }
    const QString typed = edit->text().trimmed();
    if (typed == entry.shown.trimmed() && !entry.refused) {
        return;  // Focus left without a change.
    }
    QString error;
    const std::optional<Json> value = parse(entry, typed, error);
    if (!value) {
        entry.refused = error;
        show_state(entry);
        return;
    }
    if (!value->is_null()) {
        if (const auto problem = form::check_value(*entry.field, *value)) {
            entry.refused =
                to_qstring(form::text(*problem, localization_, source_->owner(*entry.field)));
            show_state(entry);
            return;
        }
    }
    write(entry, *value);
}

void FormWidget::write(Entry& entry, const Json& value) {
    const std::string pointer = entry.field->pointer;
    const QString error = source_->write(*entry.field, value);
    // Writing may have rebuilt the form (a setting that changes the language): look it up again.
    Entry* after = find(pointer);
    if (after == nullptr) {
        return;
    }
    if (!error.isEmpty()) {
        after->refused = error;
        show_state(*after);
        return;
    }
    after->refused.reset();
    show_value(*after);
    Q_EMIT written(to_qstring(pointer));
}

void FormWidget::unit_changed(Entry& entry) {
    if (entry.units) {
        const QString id = entry.units->currentData().toString();
        units_[remembered_key(*entry.field)] = id.toStdString();
    }
    entry.refused.reset();
    show_value(entry);
}

std::optional<Json> FormWidget::parse(const Entry& entry, const QString& typed,
                                      QString& error) const {
    using form::FieldKind;
    const form::Field& field = *entry.field;
    if (typed.isEmpty()) {
        return Json();
    }
    switch (field.kind) {
        case FieldKind::integer: {
            bool ok = false;
            const qlonglong number = typed.toLongLong(&ok);
            if (!ok) {
                error = text("form.notInteger");
                return std::nullopt;
            }
            return Json(static_cast<std::int64_t>(number));
        }
        case FieldKind::number: {
            QString normalized = typed;
            normalized.replace(QLatin1Char(','), QLatin1Char('.'));
            bool ok = false;
            double number = normalized.toDouble(&ok);
            if (!ok || !std::isfinite(number)) {
                error = text("form.notNumber");
                return std::nullopt;
            }
            if (field.quantity != nullptr) {
                if (const model::DisplayUnit* unit = display_unit(entry)) {
                    number = model::from_si(*field.quantity, source_->units(), unit->to_si(number));
                }
            }
            return Json(number);
        }
        case FieldKind::string:
        case FieldKind::path:
            return Json(typed.toStdString());
        default:
            break;
    }
    return std::nullopt;
}

FormWidget::Entry* FormWidget::entry_at(std::uint64_t generation, std::size_t index) {
    return generation == generation_ && index < entries_.size() ? &entries_[index] : nullptr;
}

FormWidget::Entry* FormWidget::find(std::string_view pointer) {
    for (Entry& entry : entries_) {
        if (entry.field->pointer == pointer) {
            return &entry;
        }
    }
    return nullptr;
}

const FormWidget::Entry* FormWidget::find(std::string_view pointer) const {
    for (const Entry& entry : entries_) {
        if (entry.field->pointer == pointer) {
            return &entry;
        }
    }
    return nullptr;
}

QWidget* FormWidget::editor(std::string_view pointer) const {
    const Entry* entry = find(pointer);
    return entry != nullptr ? entry->editor.data() : nullptr;
}

QComboBox* FormWidget::unit_box(std::string_view pointer) const {
    const Entry* entry = find(pointer);
    return entry != nullptr ? entry->units.data() : nullptr;
}

QToolButton* FormWidget::advanced_button(std::string_view group) const {
    const auto found = advanced_buttons_.find(group);
    return found != advanced_buttons_.end() ? found->second.data() : nullptr;
}

form::Severity FormWidget::field_state(std::string_view pointer) const {
    const Entry* entry = find(pointer);
    return entry != nullptr ? entry->state : form::Severity::ok;
}

QString FormWidget::field_problem(std::string_view pointer) const {
    const Entry* entry = find(pointer);
    return entry != nullptr ? entry->problem : QString();
}

QString FormWidget::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(localization_, key, args);
}

QString FormWidget::field_text(const form::Field& field, const std::string& text) const {
    return to_qstring(localization_.manifest_text(source_->owner(field), text));
}

QString FormWidget::tooltip(const Entry& entry) const {
    const form::Field& field = *entry.field;
    QStringList lines;
    if (!field.description.empty()) {
        lines.push_back(field_text(field, field.description));
    }
    if (field.quantity != nullptr) {
        lines.push_back(text(
            "form.quantity",
            {{"quantity",
              localization_.text(model::core_owner, "quantity." + std::string(field.quantity->id))},
             {"unit", model_unit_label(*field.quantity).toStdString()}}));
    }
    if (!entry.problem.isEmpty()) {
        lines.push_back(entry.problem);
    }
    return lines.join(QStringLiteral("\n"));
}

const model::DisplayUnit* FormWidget::display_unit(const Entry& entry) const {
    if (!entry.units || entry.field->quantity == nullptr) {
        return nullptr;
    }
    const QString id = entry.units->currentData().toString();
    return id.isEmpty() ? nullptr : entry.field->quantity->find_unit(id.toStdString());
}

QString FormWidget::unit_label(const model::DisplayUnit& unit) const {
    return to_qstring(localization_.text(model::core_owner, "unit." + std::string(unit.id)));
}

QString FormWidget::model_unit_label(const model::Quantity& quantity) const {
    const model::UnitSystem units = source_->units();
    if (const model::DisplayUnit* own = model::model_unit(quantity, units)) {
        return unit_label(*own);
    }
    return to_qstring(model::compose_symbol(quantity.dimension, units));
}

QString FormWidget::format_number(const Entry& entry, double model_value) const {
    double shown = model_value;
    if (const model::DisplayUnit* unit = display_unit(entry)) {
        shown = unit->from_si(model::to_si(*entry.field->quantity, source_->units(), model_value));
    }
    // Twelve significant digits: what was typed comes back without the noise of conversions.
    return QString::number(shown, 'g', 12);
}

std::string FormWidget::remembered_key(const form::Field& field) const {
    return context_ + "\n" + field.pointer;
}

}  // namespace fem::shell
