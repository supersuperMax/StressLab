#pragma once

#include "fem/core/form/status.h"
#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"
#include "fem/shell/form_widget.h"
#include "fem/shell/workbench.h"

#include <QWidget>
#include <memory>
#include <string_view>

class QLabel;
class QScrollArea;

namespace fem::shell {

/// The zone "Properties" (docs/interface-design.md §4): the form of the object selected in the
/// model tree, drawn from the `propertySchemas` entry of its owner and type. A field changed in it
/// runs `model.set_property` on the Command Bus, so it is undone and journaled like any command;
/// the form follows the model when it changes, by undo too.
///
/// Object names: the heading "propertiesTitle", the type under it "propertiesType", the text shown
/// instead of a form "propertiesEmpty", the form "propertiesForm".
class PropertyPanel : public QWidget {
    Q_OBJECT

public:
    explicit PropertyPanel(Workbench& workbench, QWidget* parent = nullptr);
    ~PropertyPanel() override;
    PropertyPanel(const PropertyPanel&) = delete;
    PropertyPanel& operator=(const PropertyPanel&) = delete;
    PropertyPanel(PropertyPanel&&) = delete;
    PropertyPanel& operator=(PropertyPanel&&) = delete;

    /// Shows the form of an object; an invalid id (or an object not in the document) shows none.
    void show_object(model::ObjectId id);
    [[nodiscard]] model::ObjectId object() const noexcept { return object_; }
    [[nodiscard]] FormWidget& form() const noexcept { return *form_; }
    /// The heading: the title of the object.
    [[nodiscard]] QString title() const;
    /// What shows instead of a form, if anything.
    [[nodiscard]] QString empty_text() const;

private:
    class Source;

    void rebuild();
    void update_heading();
    [[nodiscard]] QString text(std::string_view key) const;

    Workbench& workbench_;
    form::FormIndex forms_;
    model::ObjectId object_;
    std::unique_ptr<Source> source_;
    QLabel* title_ = nullptr;
    QLabel* type_ = nullptr;
    QLabel* empty_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    FormWidget* form_ = nullptr;
    Subscription model_changed_;
    Subscription extension_events_;
    Subscription language_;
};

}  // namespace fem::shell
