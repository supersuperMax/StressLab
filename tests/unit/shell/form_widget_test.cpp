// The generator of forms (task 1.13) on the fixtures of tests/data/forms: an editor for every kind
// of field, units next to values that switch what is shown and not the value, required empty
// fields highlighted, "Advanced" folded, and only values that fit are written.

#include "fem/shell/form_widget.h"

#include "fem/core/form/form.h"
#include "fem/core/l10n/catalog.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/units.h"
#include "fem/shell/strings.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using fem::form::Field;
using fem::form::FormSpec;
using fem::form::Severity;
using fem::shell::FormSource;
using fem::shell::FormWidget;
using fem::shell::testing::q;
using fem::shell::testing::utf8;
using Json = nlohmann::json;

constexpr const char* owner = "test.module";

FormSpec fixture(const std::string& name) {
    const std::filesystem::path file =
        std::filesystem::path(FEM_TEST_DATA_DIR) / "forms" / (name + ".schema.json");
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return fem::form::parse(Json::parse(text.str()));
}

/// Values in memory; what the form writes is recorded.
class MemorySource final : public FormSource {
public:
    Json values = Json::object();
    std::vector<std::pair<std::string, Json>> writes;
    fem::model::UnitSystem system = fem::model::UnitSystem::si();
    QString refuse;  ///< A message to refuse every write with.

    [[nodiscard]] Json value(const Field& field) const override {
        const Json::json_pointer pointer("/" + field.pointer);
        return values.contains(pointer) ? values.at(pointer) : Json();
    }
    QString write(const Field& field, const Json& value) override {
        writes.emplace_back(field.pointer, value);
        if (!refuse.isEmpty()) {
            return refuse;
        }
        const Json::json_pointer pointer("/" + field.pointer);
        if (value.is_null()) {
            if (values.contains(pointer)) {
                values.at(pointer.parent_pointer()).erase(pointer.back());
            }
        } else {
            values[pointer] = value;
        }
        return {};
    }
    [[nodiscard]] std::string owner(const Field& /*field*/) const override { return ::owner; }
    [[nodiscard]] fem::model::UnitSystem units() const override { return system; }
};

class FormWidgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        fem::shell::add_shell_catalogs(localization_);
        localization_.add_catalog(owner, "en",
                                  fem::l10n::parse_catalog(R"({"format": "stresslab.l10n",
                "formatVersion": 1, "strings": {"material.E": "Young's modulus",
                "solver.method": "Method", "solver.method.direct": "Direct",
                "solver.method.iterative": "Iterative"}})"));
        localization_.add_catalog(owner, "ru",
                                  fem::l10n::parse_catalog(utf8(u8R"({"format": "stresslab.l10n",
                "formatVersion": 1, "strings": {"material.E": "Модуль Юнга"}})")));
        widget_ = std::make_unique<FormWidget>(localization_);
        widget_->resize(400, 600);
    }

    void show(const std::string& name, std::string context = "test") {
        form_ = fixture(name);
        widget_->set_form(&form_, &source_, std::move(context));
        widget_->show();
        QApplication::processEvents();
    }

    template <class T>
    T* editor(std::string_view pointer) const {
        return qobject_cast<T*>(widget_->editor(pointer));
    }

    /// Types into a text field and ends editing, as Enter does.
    void type(std::string_view pointer, const QString& text) {
        auto* edit = editor<QLineEdit>(pointer);
        ASSERT_NE(edit, nullptr) << pointer;
        edit->setFocus();
        edit->selectAll();
        QTest::keyClick(edit, Qt::Key_Delete);
        QTest::keyClicks(edit, text);
        QTest::keyClick(edit, Qt::Key_Return);
    }

    fem::l10n::Localization localization_;
    MemorySource source_;
    FormSpec form_;
    std::unique_ptr<FormWidget> widget_;
};

}  // namespace

TEST_F(FormWidgetTest, EveryKindOfFieldHasItsEditor) {
    source_.values = {{"enabled", true}, {"count", 8}, {"name", utf8(u8"Деталь")}};
    show("scalars");
    ASSERT_NE(editor<QCheckBox>("enabled"), nullptr);
    EXPECT_TRUE(editor<QCheckBox>("enabled")->isChecked());
    EXPECT_EQ(editor<QLineEdit>("count")->text(), QStringLiteral("8"));
    EXPECT_EQ(editor<QLineEdit>("name")->text(), q(utf8(u8"Деталь")));
    // A path has a button for the file dialog; a read-only field cannot be edited.
    EXPECT_NE(widget_->findChild<QToolButton*>(QStringLiteral("browse:file")), nullptr);
    EXPECT_TRUE(editor<QLineEdit>("size")->isReadOnly());
    // A default shows as a hint in an empty field.
    source_.values.erase("count");
    widget_->refresh();
    EXPECT_EQ(editor<QLineEdit>("count")->text(), QString());
    EXPECT_EQ(editor<QLineEdit>("count")->placeholderText(), QStringLiteral("4"));

    show("choices");
    auto* method = editor<QComboBox>("method");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->count(), 2);
    EXPECT_EQ(method->itemText(0), QStringLiteral("Direct"));
    EXPECT_EQ(method->currentIndex(), -1);  // Nothing chosen yet.
    EXPECT_EQ(editor<QComboBox>("order")->placeholderText(), QStringLiteral("2"));

    show("unsupported");
    for (const std::string_view pointer : {"vector", "either", "linked", "nullable", "free"}) {
        ASSERT_NE(editor<QLineEdit>(pointer), nullptr) << pointer;
        EXPECT_TRUE(editor<QLineEdit>(pointer)->isReadOnly()) << pointer;
    }
}

TEST_F(FormWidgetTest, GroupsAndTitlesInTheLanguageOfTheApplication) {
    show("groups");
    EXPECT_NE(editor<QLineEdit>("solver/tolerance"), nullptr);
    EXPECT_NE(editor<QCheckBox>("solver/a~1b"), nullptr);
    EXPECT_NE(widget_->advanced_button("solver"), nullptr);
    EXPECT_NE(widget_->advanced_button(""), nullptr);

    show("material");
    auto* label = qobject_cast<QLabel*>(
        qobject_cast<QFormLayout*>(widget_->editor("E")->parentWidget()->parentWidget()->layout())
            ->labelForField(widget_->editor("E")->parentWidget()));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QStringLiteral("Young's modulus *"));  // Required: starred.
    localization_.set_language("ru");
    widget_->rebuild();
    EXPECT_EQ(widget_->advanced_button()->text(), q(utf8(u8"Дополнительно")));
}

TEST_F(FormWidgetTest, UnitsShowNextToTheValueAndSwitchWhatIsShown) {
    source_.system = fem::model::UnitSystem::mm_t_s();
    source_.values = {{"E", 210000.0}, {"nu", 0.3}, {"density", 7.85e-9}, {"reference", 20.0}};
    show("material", "test.module\nmaterial");
    QComboBox* units = widget_->unit_box("E");
    ASSERT_NE(units, nullptr);
    EXPECT_EQ(units->currentData().toString(), QStringLiteral("MPa"));  // The unit of the model.
    EXPECT_EQ(units->currentText(), QStringLiteral("MPa"));
    EXPECT_EQ(editor<QLineEdit>("E")->text(), QStringLiteral("210000"));
    EXPECT_EQ(widget_->unit_box("density")->currentData().toString(), QStringLiteral("t/mm3"));
    EXPECT_EQ(editor<QLineEdit>("density")->text(), QStringLiteral("7.85e-09"));
    EXPECT_EQ(widget_->unit_box("nu"), nullptr);  // No quantity, no unit.

    units->setCurrentIndex(units->findData(QStringLiteral("GPa")));
    EXPECT_EQ(editor<QLineEdit>("E")->text(), QStringLiteral("210"));
    EXPECT_TRUE(source_.writes.empty());  // Showing in another unit changes no value.
    // A value typed in the unit shown is written in the units of the model.
    type("E", QStringLiteral("200"));
    ASSERT_EQ(source_.writes.size(), 1U);
    EXPECT_EQ(source_.writes[0].first, "E");
    EXPECT_DOUBLE_EQ(source_.writes[0].second.get<double>(), 200000.0);
    EXPECT_EQ(editor<QLineEdit>("E")->text(), QStringLiteral("200"));

    // Absolute temperatures convert with their offset.
    QComboBox* temperature = widget_->unit_box("reference");
    temperature->setCurrentIndex(temperature->findData(QStringLiteral("K")));
    EXPECT_EQ(editor<QLineEdit>("reference")->text(), QStringLiteral("293.15"));

    // The unit picked stays for the session, for the same context.
    show("material", "test.module\nmaterial");
    EXPECT_EQ(widget_->unit_box("E")->currentData().toString(), QStringLiteral("GPa"));
    show("material", "other\nmaterial");
    EXPECT_EQ(widget_->unit_box("E")->currentData().toString(), QStringLiteral("MPa"));
}

TEST_F(FormWidgetTest, AUnitOutsideTheCatalogIsComposedOfTheBaseUnits) {
    source_.system = {fem::model::LengthUnit::inch, fem::model::MassUnit::kilogram,
                      fem::model::TimeUnit::second, fem::model::TemperatureUnit::kelvin};
    source_.values = {{"E", 1.0}};
    show("material");
    QComboBox* units = widget_->unit_box("E");
    EXPECT_EQ(units->currentData().toString(), QString());
    EXPECT_EQ(units->currentText(), q(utf8(u8"kg·in⁻¹·s⁻²")));
    EXPECT_EQ(editor<QLineEdit>("E")->text(), QStringLiteral("1"));
    units->setCurrentIndex(units->findData(QStringLiteral("Pa")));
    EXPECT_EQ(editor<QLineEdit>("E")->text(), QStringLiteral("39.3700787402"));  // 1 / 0.0254
}

TEST_F(FormWidgetTest, RequiredEmptyFieldsAreHighlighted) {
    show("material");
    EXPECT_EQ(widget_->field_state("E"), Severity::error);
    EXPECT_EQ(widget_->editor("E")->property("problem").toString(), QStringLiteral("error"));
    EXPECT_EQ(widget_->field_problem("E"), q(utf8(u8"“Young's modulus” is required and not set.")));
    EXPECT_TRUE(widget_->editor("E")->toolTip().contains(widget_->field_problem("E")));
    EXPECT_EQ(widget_->field_state("density"), Severity::ok);  // Not required.
    EXPECT_EQ(widget_->editor("density")->property("problem").toString(), QString());

    type("E", QStringLiteral("2.1e11"));
    EXPECT_EQ(widget_->field_state("E"), Severity::ok);
    EXPECT_EQ(widget_->editor("E")->property("problem").toString(), QString());
}

TEST_F(FormWidgetTest, TheAdvancedPartIsFolded) {
    show("material");
    QToolButton* advanced = widget_->advanced_button();
    ASSERT_NE(advanced, nullptr);
    EXPECT_FALSE(advanced->isChecked());
    EXPECT_FALSE(widget_->editor("reference")->isVisible());
    EXPECT_TRUE(widget_->editor("E")->isVisible());
    QTest::mouseClick(advanced, Qt::LeftButton);
    EXPECT_TRUE(widget_->editor("reference")->isVisible());
    // It stays open for this context in the session.
    show("material");
    EXPECT_TRUE(widget_->advanced_button()->isChecked());
    EXPECT_TRUE(widget_->editor("reference")->isVisible());
    show("material", "another");
    EXPECT_FALSE(widget_->editor("reference")->isVisible());
}

TEST_F(FormWidgetTest, WhatDoesNotFitIsNotWritten) {
    source_.values = {{"count", 4}};
    show("scalars");
    const QSignalSpy written(widget_.get(), &FormWidget::written);
    type("count", QStringLiteral("many"));
    EXPECT_TRUE(source_.writes.empty());
    EXPECT_EQ(widget_->field_state("count"), Severity::error);
    EXPECT_EQ(widget_->field_problem("count"), QStringLiteral("Not an integer."));
    EXPECT_EQ(editor<QLineEdit>("count")->text(), QStringLiteral("many"));  // Kept to correct.

    type("count", QStringLiteral("0"));
    EXPECT_TRUE(source_.writes.empty());
    EXPECT_EQ(widget_->field_problem("count"), q(utf8(u8"“Count” must be at least 1.")));

    type("count", QStringLiteral("12"));
    ASSERT_EQ(source_.writes.size(), 1U);
    EXPECT_EQ(source_.writes[0].second, Json(12));
    EXPECT_EQ(widget_->field_state("count"), Severity::ok);
    ASSERT_EQ(written.count(), 1);
    EXPECT_EQ(written.front().front().toString(), QStringLiteral("count"));

    // Leaving a field without a change writes nothing.
    editor<QLineEdit>("count")->setFocus();
    Q_EMIT editor<QLineEdit>("count")->editingFinished();
    EXPECT_EQ(source_.writes.size(), 1U);
}

TEST_F(FormWidgetTest, ADecimalCommaIsReadAsAPoint) {
    show("material");
    type("nu", QStringLiteral("0,25"));
    ASSERT_EQ(source_.writes.size(), 1U);
    EXPECT_DOUBLE_EQ(source_.writes[0].second.get<double>(), 0.25);
}

TEST_F(FormWidgetTest, AnEmptiedFieldRemovesItsValue) {
    source_.values = {{"name", "a"}};
    show("scalars");
    type("name", QString());
    ASSERT_EQ(source_.writes.size(), 1U);
    EXPECT_TRUE(source_.writes[0].second.is_null());
    EXPECT_FALSE(source_.values.contains("name"));
}

TEST_F(FormWidgetTest, CheckBoxesAndListsWriteAtOnce) {
    show("choices");
    auto* method = editor<QComboBox>("method");
    method->setCurrentIndex(1);
    Q_EMIT method->activated(1);
    ASSERT_EQ(source_.writes.size(), 1U);
    EXPECT_EQ(source_.writes[0], (std::pair<std::string, Json>{"method", "iterative"}));
    EXPECT_EQ(widget_->field_state("method"), Severity::ok);

    show("scalars");
    editor<QCheckBox>("enabled")->click();
    ASSERT_EQ(source_.writes.size(), 2U);
    EXPECT_EQ(source_.writes[1], (std::pair<std::string, Json>{"enabled", true}));
}

TEST_F(FormWidgetTest, AWriteTheSourceRefusesKeepsTheFieldHighlighted) {
    source_.refuse = QStringLiteral("no way");
    source_.values = {{"name", "a"}};
    show("scalars");
    type("name", QStringLiteral("b"));
    EXPECT_EQ(widget_->field_state("name"), Severity::error);
    EXPECT_EQ(widget_->field_problem("name"), QStringLiteral("no way"));
    // When the value is shown again, the field is as the source has it.
    widget_->refresh();
    EXPECT_EQ(editor<QLineEdit>("name")->text(), QStringLiteral("a"));
    EXPECT_EQ(widget_->field_state("name"), Severity::ok);
}

TEST_F(FormWidgetTest, ValuesThatDoNotFitTheSchemaAreShownAsWrong) {
    source_.values = {{"E", -5.0}, {"nu", "high"}};
    show("material");
    EXPECT_EQ(widget_->field_state("E"), Severity::error);
    EXPECT_EQ(widget_->field_problem("E"), q(utf8(u8"“Young's modulus” must be greater than 0.")));
    EXPECT_EQ(widget_->field_state("nu"), Severity::error);
    EXPECT_EQ(editor<QLineEdit>("nu")->text(), QStringLiteral("high"));
}
