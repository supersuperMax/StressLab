// The settings form (task 1.13): the same generator as the properties, over the settings of the
// core and of enabled modules; a change goes to the level of the user and is saved at once.

#include "fem/shell/settings_panel.h"

#include "fem/core/bindings.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/form_widget.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <memory>
#include <string>
#include <vector>

namespace {

using fem::form::FieldKind;
using fem::settings::Level;
using fem::shell::SettingsPanel;
using fem::shell::testing::q;
using fem::shell::testing::utf8;

class SettingsPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        app_.add("test.solver", R"(
[contributes.settings]
"solver.threads" = { type = "integer", default = 0, minimum = 0, maximum = 64, title = "Threads", description = "0 means one per core" }
"solver.mode" = { type = "string", default = "fast", enum = ["fast", "exact"] }
)");
        panel_ = std::make_unique<SettingsPanel>(workbench_);
        panel_->resize(400, 600);
        panel_->show();
        QApplication::processEvents();
    }

    void type(std::string_view key, const QString& text) {
        auto* edit = qobject_cast<QLineEdit*>(panel_->form().editor(key));
        ASSERT_NE(edit, nullptr) << key;
        edit->setFocus();
        edit->selectAll();
        QTest::keyClick(edit, Qt::Key_Delete);
        QTest::keyClicks(edit, text);
        QTest::keyClick(edit, Qt::Key_Return);
    }

    fem::shell::testing::Application app_;
    // The language follows its setting, as in the application; the system speaks English here.
    fem::Subscription language_ = fem::bind_language(app_.settings, app_.localization, "en");
    fem::shell::Workbench workbench_ = app_.workbench();
    std::unique_ptr<SettingsPanel> panel_;
};

}  // namespace

TEST_F(SettingsPanelTest, OneSectionForTheCoreThenOnePerModule) {
    const fem::form::FormSpec& spec = panel_->spec();
    ASSERT_EQ(spec.fields.size(), 2U);
    EXPECT_EQ(spec.fields[0].kind, FieldKind::section);
    EXPECT_EQ(spec.fields[0].title, "General");
    EXPECT_EQ(spec.fields[1].title, "Module test.solver");
    std::vector<std::string> keys;
    for (const fem::form::Field& field : spec.fields[1].fields) {
        keys.push_back(field.key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"solver.mode", "solver.threads"}));
    // Every setting has its editor, with the value in effect.
    EXPECT_EQ(qobject_cast<QLineEdit*>(panel_->form().editor("history.undoLimit"))->text(),
              QStringLiteral("100"));
    auto* mode = qobject_cast<QComboBox*>(panel_->form().editor("solver.mode"));
    ASSERT_NE(mode, nullptr);
    EXPECT_EQ(mode->currentText(), QStringLiteral("fast"));
    EXPECT_NE(panel_->form().editor("general.language"), nullptr);
    EXPECT_NE(panel_->form().editor("log.level"), nullptr);
}

TEST_F(SettingsPanelTest, AChangeGoesToTheUserAndIsSaved) {
    type("solver.threads", QStringLiteral("8"));
    EXPECT_EQ(app_.settings.stored(Level::user, "solver.threads"), nlohmann::json(8));
    EXPECT_EQ(app_.settings_saved, 1);
    // A value outside the limits is not written.
    type("solver.threads", QStringLiteral("100"));
    EXPECT_EQ(app_.settings.stored(Level::user, "solver.threads"), nlohmann::json(8));
    EXPECT_EQ(panel_->form().field_state("solver.threads"), fem::form::Severity::error);
    EXPECT_EQ(app_.settings_saved, 1);
    // An emptied field goes back to the default.
    type("solver.threads", QString());
    EXPECT_FALSE(app_.settings.stored(Level::user, "solver.threads"));
    EXPECT_EQ(qobject_cast<QLineEdit*>(panel_->form().editor("solver.threads"))->text(),
              QStringLiteral("0"));
    EXPECT_EQ(app_.settings_saved, 2);
}

TEST_F(SettingsPanelTest, TheFormFollowsSettingsChangedElsewhere) {
    app_.settings.set(Level::user, "history.undoLimit", 5);
    EXPECT_EQ(qobject_cast<QLineEdit*>(panel_->form().editor("history.undoLimit"))->text(),
              QStringLiteral("5"));
    app_.settings.set(Level::project, "solver.threads", 4);
    EXPECT_EQ(qobject_cast<QLineEdit*>(panel_->form().editor("solver.threads"))->text(),
              QStringLiteral("4"));
    // The note says the project sets it.
    bool noted = false;
    for (const QLabel* label : panel_->findChildren<QLabel*>()) {
        noted = noted ||
                (label->isVisible() &&
                 label->text() == QStringLiteral("The project sets 4, and that is in effect."));
    }
    EXPECT_TRUE(noted);
}

TEST_F(SettingsPanelTest, SectionsFollowTheModulesAndTheLanguage) {
    (void)app_.manager.disable("test.solver");
    EXPECT_EQ(panel_->spec().fields.size(), 1U);
    EXPECT_EQ(panel_->form().editor("solver.threads"), nullptr);
    (void)app_.manager.enable("test.solver");
    EXPECT_NE(panel_->form().editor("solver.threads"), nullptr);

    // The language is a setting too: changing it in the form translates the form.
    type("general.language", QStringLiteral("ru"));
    EXPECT_EQ(app_.settings.value("general.language"), "ru");
    EXPECT_EQ(panel_->spec().fields[0].title, utf8(u8"Общие"));
    EXPECT_NE(panel_->form().editor("general.language"), nullptr);
}
