// The model tree and the properties (task 1.13): the tree is built from the document and the
// treeNodeTypes of modules, shows ⚠/✕, filters, has a context menu per node; the properties show
// the form of the selected object, and a field changed runs a command that undoes and is journaled.

#include "fem/shell/model_tree.h"

#include "fem/core/command/model_commands.h"
#include "fem/shell/commands.h"
#include "fem/shell/form_widget.h"
#include "fem/shell/main_window.h"
#include "fem/shell/property_panel.h"
#include "fem/shell/theme.h"

#include "shell_test_support.h"
#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTest>
#include <QTreeView>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::command::Value;
using fem::form::Severity;
using fem::model::ObjectId;
using fem::shell::MainWindow;
using fem::shell::ModelTreeModel;
using fem::shell::testing::q;
using fem::shell::testing::shown_items;
using fem::shell::testing::utf8;
namespace mc = fem::command::model_commands;

constexpr const char* structural = "test.structural";

/// A module with two object types, a form for materials, a context-menu item for them and a
/// command that takes the selected object.
constexpr std::string_view module = R"(
[[contributes.commands]]
id = "structural.check"
title = "Check Material"
args = [{ name = "object", type = "object" }]
[[contributes.commands]]
id = "structural.addMaterial"
title = "Add Material"
[[contributes.treeNodeTypes]]
type = "material"
title = "Material"
[[contributes.treeNodeTypes]]
type = "materials"
title = "Materials"
[[contributes.propertySchemas]]
type = "material"
version = 1
schema = { type = "object", required = ["E"], properties = { E = { type = "number", title = "E", x-quantity = "pressure", exclusiveMinimum = 0 }, nu = { type = "number", title = "nu", maximum = 0.5 } } }
[[contributes.contextMenus]]
location = "tree"
command = "structural.check"
when = "tree.selectedNodeType == 'material'"
[[contributes.contextMenus]]
location = "tree"
command = "structural.addMaterial"
when = "tree.selectedNodeType == 'materials' || tree.selectedNodeType == ''"
)";

class ModelTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        fem::shell::apply_theme(*qApp, Qt::ColorScheme::Light);
        app_.add(structural, module);
        window_ = std::make_unique<MainWindow>(app_.workbench());
        window_->show();
        QApplication::processEvents();
    }

    ObjectId create(std::string type, std::string name, ObjectId parent = {},
                    Value::Dict properties = {}) {
        Value::Dict args{{"type", std::move(type)},
                         {"owner", structural},
                         {"name", std::move(name)},
                         {"properties", std::move(properties)}};
        if (parent.is_valid()) {
            args.emplace("parent", Value(parent));
        }
        return ObjectId(
            static_cast<std::uint64_t>(app_.bus.execute(mc::create, args).value.as_integer()));
    }

    [[nodiscard]] fem::shell::ModelTreePanel& tree() const { return window_->model_tree(); }
    [[nodiscard]] ModelTreeModel& model() const { return tree().model(); }

    /// The titles of the rows the view shows, depth first, indented by level.
    [[nodiscard]] QStringList rows() const {
        QStringList out;
        const QAbstractItemModel& proxy = tree().proxy();
        const std::function<void(const QModelIndex&, int)> visit = [&](const QModelIndex& parent,
                                                                       int level) {
            for (int row = 0; row < proxy.rowCount(parent); ++row) {
                const QModelIndex index = proxy.index(row, 0, parent);
                out.push_back(QString(level * 2, QLatin1Char(' ')) + index.data().toString());
                visit(index, level + 1);
            }
        };
        visit({}, 0);
        return out;
    }

    [[nodiscard]] Severity status(ObjectId id) const {
        return static_cast<Severity>(model().index_of(id).data(ModelTreeModel::StatusRole).toInt());
    }

    fem::shell::testing::Application app_;
    std::unique_ptr<MainWindow> window_;
};

}  // namespace

TEST_F(ModelTreeTest, ShowsTheObjectsWithTheTitlesOfTheirTypes) {
    EXPECT_TRUE(rows().isEmpty());
    const ObjectId materials = create("materials", "");
    create("material", utf8(u8"Сталь 20"), materials, {{"E", 2.1e11}});
    create("material", "", materials, {{"E", 7e10}});
    create("other", "Loose");
    EXPECT_EQ(rows(), QStringList({"Materials", q(utf8(u8"  Сталь 20")), "  Material", "Loose"}));
    // The type and the module are in the tooltip.
    EXPECT_TRUE(model()
                    .index_of(materials)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .startsWith(QStringLiteral("Materials — module “Module test.structural”")));
    EXPECT_FALSE(model().index_of(materials).data(Qt::DecorationRole).value<QIcon>().isNull());
}

TEST_F(ModelTreeTest, MarksShowWhatIsWrongAndFollowTheModel) {
    const ObjectId materials = create("materials", "");
    const ObjectId steel = create("material", "Steel", materials);
    EXPECT_EQ(status(steel), Severity::error);      // E is required.
    EXPECT_EQ(status(materials), Severity::error);  // The worst inside.
    EXPECT_EQ(model().index_of(materials).data(ModelTreeModel::OwnStatusRole).toInt(),
              static_cast<int>(Severity::ok));
    const QModelIndex mark = model().index_of(steel).siblingAtColumn(ModelTreeModel::status_column);
    EXPECT_FALSE(mark.data(Qt::DecorationRole).value<QIcon>().isNull());
    EXPECT_TRUE(
        mark.data(Qt::ToolTipRole).toString().contains(q(utf8(u8"“E” is required and not set."))));

    app_.bus.execute(mc::set_property, {{"object", steel}, {"key", "E"}, {"value", 2.1e11}});
    EXPECT_EQ(status(steel), Severity::ok);
    EXPECT_EQ(status(materials), Severity::ok);
    EXPECT_TRUE(model()
                    .index_of(steel)
                    .siblingAtColumn(ModelTreeModel::status_column)
                    .data(Qt::DecorationRole)
                    .value<QIcon>()
                    .isNull());
    ASSERT_TRUE(app_.bus.undo());
    EXPECT_EQ(status(steel), Severity::error);

    // A dependency on a removed object is an error too.
    const ObjectId other = create("material", "Other", materials, {{"E", 1.0}});
    app_.bus.execute(mc::add_dependency, {{"object", other}, {"target", steel}});
    app_.bus.execute(mc::remove, {{"object", steel}});
    EXPECT_EQ(status(other), Severity::error);
}

TEST_F(ModelTreeTest, ObjectsOfADisabledModuleHideAndComeBack) {
    const ObjectId materials = create("materials", "");
    create("material", "Steel", materials, {{"E", 1.0}});
    tree().view()->expandAll();
    tree().select(materials);
    ASSERT_EQ(rows().size(), 2);
    (void)app_.manager.disable(structural);
    EXPECT_TRUE(rows().isEmpty());
    EXPECT_EQ(app_.document.size(), 2U);  // Kept in the model, opaque.
    EXPECT_FALSE(tree().selected().is_valid());
    EXPECT_FALSE(window_->properties().object().is_valid());
    (void)app_.manager.enable(structural);
    EXPECT_EQ(rows(), QStringList({"Materials", "  Steel"}));
}

TEST_F(ModelTreeTest, TheFilterFindsByNameAndByType) {
    const ObjectId materials = create("materials", "");
    create("material", "Steel", materials, {{"E", 1.0}});
    create("material", "Aluminium", materials, {{"E", 1.0}});
    create("other", "Loose");
    tree().filter()->setText(QStringLiteral("alu"));
    EXPECT_EQ(rows(), QStringList({"Materials", "  Aluminium"}));
    tree().filter()->setText(QStringLiteral("MATERIAL"));  // A type matches with what it holds.
    EXPECT_EQ(rows(), QStringList({"Materials", "  Steel", "  Aluminium"}));
    tree().filter()->clear();
    EXPECT_EQ(rows().size(), 4);
}

TEST_F(ModelTreeTest, TheSelectionSetsTheKeysAndShowsTheForm) {
    const ObjectId materials = create("materials", "");
    const ObjectId steel = create("material", "Steel", materials, {{"E", 2.1e11}});
    fem::shell::PropertyPanel& properties = window_->properties();
    EXPECT_EQ(properties.empty_text(),
              QStringLiteral("Select an object in the model tree to see its properties."));

    tree().select(steel);
    EXPECT_EQ(app_.keys.value("tree.selectedNodeType").as_string(), "material");
    EXPECT_EQ(app_.keys.value("tree.selectedNodeOwner").as_string(), structural);
    EXPECT_EQ(properties.object(), steel);
    EXPECT_EQ(properties.title(), QStringLiteral("Steel"));
    ASSERT_NE(properties.form().editor("E"), nullptr);
    EXPECT_EQ(qobject_cast<QLineEdit*>(properties.form().editor("E"))->text(),
              QStringLiteral("210000000000"));

    tree().select(materials);  // A type without a form.
    EXPECT_EQ(app_.keys.value("tree.selectedNodeType").as_string(), "materials");
    EXPECT_EQ(properties.empty_text(),
              QStringLiteral("The module of this object describes no properties for it."));
    tree().select({});
    EXPECT_EQ(app_.keys.value("tree.selectedNodeType").as_string(), "");
}

TEST_F(ModelTreeTest, AFieldChangedIsACommandThatUndoesAndIsJournaled) {
    std::vector<std::string> journal;
    const auto subscription =
        app_.bus.on_journal([&journal](std::string_view entry) { journal.emplace_back(entry); });
    const ObjectId steel = create("material", "Steel", {}, {{"E", 2.1e11}});
    tree().select(steel);
    fem::shell::FormWidget& form = window_->properties().form();
    auto* edit = qobject_cast<QLineEdit*>(form.editor("E"));
    ASSERT_NE(edit, nullptr);
    edit->setFocus();
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("2e11"));
    QTest::keyClick(edit, Qt::Key_Return);

    EXPECT_EQ(app_.document.get(steel).properties().at("E"), 2e11);
    ASSERT_FALSE(journal.empty());
    EXPECT_EQ(journal.back(),
              "stresslab.cmd(\"model.set_property\", object=1, key=\"E\", value=200000000000.0)");
    EXPECT_EQ(app_.bus.undo_label(), mc::set_property);

    // Undo from the "Edit" menu: the form follows the model.
    window_->findChild<QAction*>(QStringLiteral("app.undo"))->trigger();
    EXPECT_EQ(app_.document.get(steel).properties().at("E"), 2.1e11);
    EXPECT_EQ(qobject_cast<QLineEdit*>(form.editor("E"))->text(), QStringLiteral("210000000000"));

    // An emptied required field removes the value: the field and the node show the error.
    edit = qobject_cast<QLineEdit*>(form.editor("E"));
    edit->setFocus();
    edit->selectAll();
    QTest::keyClick(edit, Qt::Key_Delete);
    QTest::keyClick(edit, Qt::Key_Return);
    EXPECT_FALSE(app_.document.get(steel).properties().contains("E"));
    EXPECT_EQ(form.field_state("E"), Severity::error);
    EXPECT_EQ(status(steel), Severity::error);
}

TEST_F(ModelTreeTest, TheFormOfAnObjectFollowsItsRemoval) {
    const ObjectId steel = create("material", "Steel", {}, {{"E", 1.0}});
    tree().select(steel);
    app_.bus.execute(mc::remove, {{"object", steel}});
    EXPECT_FALSE(window_->properties().object().is_valid());
    EXPECT_FALSE(tree().selected().is_valid());
    ASSERT_TRUE(app_.bus.undo());
    tree().select(steel);
    EXPECT_EQ(window_->properties().object(), steel);
}

TEST_F(ModelTreeTest, TheContextMenuHasTheItemsOfModulesForTheNodeAndOfTheShell) {
    const ObjectId materials = create("materials", "");
    const ObjectId steel = create("material", "Steel", materials, {{"E", 1.0}});

    std::unique_ptr<QMenu> menu(tree().context_menu());
    ASSERT_NE(menu, nullptr);  // Nothing selected: what modules offer there.
    EXPECT_EQ(shown_items(menu.get()), QStringList({"Add Material"}));

    tree().select(steel);
    menu.reset(tree().context_menu());
    EXPECT_EQ(shown_items(menu.get()), QStringList({"Check Material", "---", "Rename", "Delete"}));
    // The command takes the selected object.
    menu->actions().front()->trigger();
    EXPECT_EQ(app_.runtime->log, std::vector<std::string>{"execute structural.check"});

    tree().select(materials);
    menu.reset(tree().context_menu());
    EXPECT_EQ(shown_items(menu.get()), QStringList({"Add Material", "---", "Rename", "Delete"}));
}

TEST_F(ModelTreeTest, CommandsTakingAnObjectNeedASelection) {
    const ObjectId steel = create("material", "Steel", {}, {{"E", 1.0}});
    fem::shell::Commands& commands = window_->commands();
    EXPECT_FALSE(commands.availability("structural.check").available);
    EXPECT_EQ(commands.availability("structural.check").reason,
              QStringLiteral("Select an object in the model tree"));
    EXPECT_FALSE(commands.run("structural.check"));
    tree().select(steel);
    EXPECT_TRUE(commands.availability("structural.check").available);
    EXPECT_TRUE(commands.run("structural.check"));
    // Model commands that take an object work from the tree as well.
    EXPECT_TRUE(commands.availability(mc::remove).available);
}

TEST_F(ModelTreeTest, NodesAreRenamedAndDeleted) {
    const ObjectId materials = create("materials", "");
    const ObjectId steel = create("material", "Steel", materials, {{"E", 1.0}});
    tree().select(steel);
    EXPECT_TRUE(model().setData(model().index_of(steel), QStringLiteral("S235"), Qt::EditRole));
    EXPECT_EQ(app_.document.get(steel).name(), "S235");
    EXPECT_EQ(window_->properties().title(), QStringLiteral("S235"));
    // A name that is taken is refused, with the reason in the prompt line.
    create("material", "Taken", materials, {{"E", 1.0}});
    EXPECT_FALSE(model().setData(model().index_of(steel), QStringLiteral("Taken"), Qt::EditRole));
    EXPECT_TRUE(window_->prompt().startsWith(QStringLiteral("Cannot rename to “Taken”")));

    tree().select(steel);
    tree().delete_action()->trigger();
    EXPECT_EQ(app_.document.find(steel), nullptr);
    EXPECT_FALSE(tree().selected().is_valid());
    ASSERT_TRUE(app_.bus.undo());
    EXPECT_NE(app_.document.find(steel), nullptr);
}

TEST_F(ModelTreeTest, ANodeNeededByADisabledModuleIsNotDeleted) {
    app_.add("test.other", R"(
[[contributes.treeNodeTypes]]
type = "load"
title = "Load"
)");
    const ObjectId steel = create("material", "Steel", {}, {{"E", 1.0}});
    app_.bus.execute(
        mc::create,
        {{"type", "load"}, {"owner", "test.other"}, {"dependencies", Value::List{Value(steel)}}});
    (void)app_.manager.disable("test.other");
    tree().select(steel);
    tree().delete_action()->trigger();
    EXPECT_NE(app_.document.find(steel), nullptr);
    EXPECT_EQ(window_->prompt(),
              QStringLiteral("“Steel” cannot be deleted: it holds or is needed by objects of the "
                             "disabled module “Module test.other”."));
}
