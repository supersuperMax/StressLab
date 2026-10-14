#pragma once

#include "fem/core/form/form.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/units.h"

#include <nlohmann/json.hpp>

#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;
class QFormLayout;
class QLabel;
class QToolButton;

namespace fem::shell {

/// Where a form reads and writes its values: the properties of an object of the model (through the
/// Command Bus) or the settings of the user.
class FormSource {
public:
    FormSource() = default;
    FormSource(const FormSource&) = delete;
    FormSource& operator=(const FormSource&) = delete;
    FormSource(FormSource&&) = delete;
    FormSource& operator=(FormSource&&) = delete;
    virtual ~FormSource() = default;

    /// The value of a field; null if it has none.
    [[nodiscard]] virtual nlohmann::json value(const form::Field& field) const = 0;
    /// Writes the value of a field; null removes it. Returns a message for the user if the value
    /// could not be written, empty if it was.
    virtual QString write(const form::Field& field, const nlohmann::json& value) = 0;
    /// The module whose catalogs translate the texts of the field written as "%key%".
    [[nodiscard]] virtual std::string owner(const form::Field& field) const = 0;
    /// The unit system values of quantities are in.
    [[nodiscard]] virtual model::UnitSystem units() const { return model::UnitSystem::si(); }
    /// A note under the field, such as where else its value is set; empty: none.
    [[nodiscard]] virtual QString note(const form::Field& /*field*/) const { return {}; }
};

/// The generator of forms (docs/architecture.md §4.2 `propertySchemas`, `settings`;
/// docs/interface-design.md §3–4; docs/adr/0014-tree-and-forms.md): draws the fields of a
/// `form::FormSpec` with the standard widgets of the shell, one generator for the properties of
/// objects and for settings.
///
/// - A check box for booleans, a list for choices, a text field for numbers, text and paths (paths
///   with a button that opens the file dialog of the system), and read-only JSON text for what a
///   form cannot show. Groups are framed, sections have a heading.
/// - A number of a physical quantity shows its unit next to it; the list of units switches what the
///   field shows, not the value in the model, and the unit picked is remembered for the session.
/// - Fields with `x-advanced` are in the folded "Advanced" part of their group.
/// - A required field that is empty and a value that does not fit are highlighted, with the reason
///   in the tooltip. What is typed and does not fit (not a number, outside the limits) is not
///   written: the field keeps the text, highlighted, until it is corrected or the value changes.
/// - Check boxes and lists write at once, text fields when editing ends (Enter or focus out), and
///   only if the text changed. An empty text field removes the value.
///
/// Object names, for tests: editors "field:<pointer>", unit lists "unit:<pointer>", the buttons of
/// "Advanced" "advanced:<pointer of the group>" ("advanced:" at the top), path buttons
/// "browse:<pointer>".
class FormWidget : public QWidget {
    Q_OBJECT

public:
    explicit FormWidget(const l10n::Localization& localization, QWidget* parent = nullptr);
    ~FormWidget() override;
    FormWidget(const FormWidget&) = delete;
    FormWidget& operator=(const FormWidget&) = delete;
    FormWidget(FormWidget&&) = delete;
    FormWidget& operator=(FormWidget&&) = delete;

    /// Shows the fields of `form` with the values of `source`; both must stay until the next call
    /// or the end of the widget. Null: no fields. `context` names the form for what is remembered
    /// in the session (units picked, "Advanced" opened), e.g. the owner and type of an object.
    void set_form(const form::FormSpec* form, FormSource* source, std::string context = {});
    /// Reads the values again, after they changed, without building the fields again.
    void refresh();
    /// Builds the fields again, e.g. for another language. The widgets of the old fields are
    /// deleted later, so a signal of one of them may be what led here.
    void rebuild();

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    /// The editor of a field: a check box, a list or a text field; nullptr if there is none.
    [[nodiscard]] QWidget* editor(std::string_view pointer) const;
    /// The list of units of a number field; nullptr if it has none.
    [[nodiscard]] QComboBox* unit_box(std::string_view pointer) const;
    /// The button that unfolds "Advanced" of a group ("" for the top); nullptr if it has none.
    [[nodiscard]] QToolButton* advanced_button(std::string_view group = {}) const;
    /// What the field shows as wrong (`ok` if nothing), and why.
    [[nodiscard]] form::Severity field_state(std::string_view pointer) const;
    [[nodiscard]] QString field_problem(std::string_view pointer) const;

Q_SIGNALS:
    /// A value was written through the source.
    void written(const QString& pointer);

private:
    struct Entry {
        const form::Field* field = nullptr;
        QPointer<QWidget> editor;
        QPointer<QLabel> label;
        QPointer<QComboBox> units;
        QPointer<QLabel> note;
        QString shown;  ///< Text last put into a text field, to write only what changed.
        /// Text typed that was not written, and why; cleared when the value is shown again.
        std::optional<QString> refused;
        form::Severity state = form::Severity::ok;
        QString problem;
    };

    void build_fields(const std::vector<form::Field>& fields, QFormLayout* layout,
                      const std::string& group);
    void add_field(const form::Field& field, QFormLayout* layout);
    QWidget* make_editor(Entry& entry, QWidget* parent);
    void show_value(Entry& entry);
    void show_state(Entry& entry);
    void edited(Entry& entry);
    void unit_changed(Entry& entry);
    void write(Entry& entry, const nlohmann::json& value);
    /// The entry a signal of a field refers to, unless the form was built again since.
    [[nodiscard]] Entry* entry_at(std::uint64_t generation, std::size_t index);
    [[nodiscard]] Entry* find(std::string_view pointer);
    [[nodiscard]] const Entry* find(std::string_view pointer) const;

    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;
    [[nodiscard]] QString field_text(const form::Field& field, const std::string& text) const;
    [[nodiscard]] QString tooltip(const Entry& entry) const;
    /// The unit a number field shows now; nullptr: the unit of the model.
    [[nodiscard]] const model::DisplayUnit* display_unit(const Entry& entry) const;
    [[nodiscard]] QString unit_label(const model::DisplayUnit& unit) const;
    [[nodiscard]] QString model_unit_label(const model::Quantity& quantity) const;
    [[nodiscard]] QString format_number(const Entry& entry, double model_value) const;
    /// What a text field holds as a value of its field; nothing with `error` set if it is none.
    [[nodiscard]] std::optional<nlohmann::json> parse(const Entry& entry, const QString& typed,
                                                      QString& error) const;
    [[nodiscard]] std::string remembered_key(const form::Field& field) const;

    const l10n::Localization& localization_;
    const form::FormSpec* form_ = nullptr;
    FormSource* source_ = nullptr;
    std::string context_;
    QWidget* content_ = nullptr;
    std::vector<Entry> entries_;
    std::uint64_t generation_ = 0;  ///< How many times the fields were built.
    std::map<std::string, QPointer<QToolButton>, std::less<>> advanced_buttons_;
    /// Units picked by the user, by context and field: unit ids, "" for the unit of the model.
    std::map<std::string, std::string, std::less<>> units_;
    /// "Advanced" parts opened, by context and group.
    std::set<std::string, std::less<>> opened_;
};

}  // namespace fem::shell
