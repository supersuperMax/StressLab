#pragma once

#include "fem/core/form/status.h"
#include "fem/core/model/object_id.h"
#include "fem/core/signal.h"
#include "fem/shell/workbench.h"

#include <QAbstractItemModel>
#include <QIcon>
#include <QString>
#include <QWidget>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QAction;
class QLineEdit;
class QMenu;
class QSortFilterProxyModel;
class QTreeView;

namespace fem::shell {

class Commands;

/// Context keys of the model tree (owner "fem.shell", text): the type and the owner of the selected
/// object, empty when nothing is selected. `contextMenus` with `location = "tree"` and other
/// places use them in `when` (docs/adr/0014-tree-and-forms.md).
inline constexpr std::string_view selected_node_type_key = "tree.selectedNodeType";
inline constexpr std::string_view selected_node_owner_key = "tree.selectedNodeOwner";

/// Declares the keys of the model tree in `keys` unless they are declared.
void declare_tree_keys(context::ContextKeys& keys);

/// The title of an object type for users: the `title` of its `treeNodeTypes` entry, a title of the
/// shell for the types of the core, else the type itself.
[[nodiscard]] QString type_title(const Workbench& workbench, std::string_view owner,
                                 std::string_view type);
/// The icon of an object type: the `icon` of its `treeNodeTypes` entry or the default module icon.
[[nodiscard]] QIcon type_icon(const Workbench& workbench, std::string_view owner,
                              std::string_view type);
/// The title of an object for users: its name, or the title of its type if it has none.
[[nodiscard]] QString object_title(const Workbench& workbench, const model::Object& object);

/// The model tree (docs/interface-design.md §3–4): the objects of the document as they nest, with
/// the title and icon of their type and their status. Objects of disabled modules (opaque) are
/// left out with everything under them, and come back when their module is enabled.
///
/// Column 0 holds the title and the icon of the type; column 1 the mark of the status (⚠/✕), the
/// worst of the object and of everything under it. Names are edited in column 0 with
/// `model.rename`.
///
/// Changes of properties and names update their rows; created and removed objects, enabled and
/// disabled modules and another language build the tree again (a reset).
class ModelTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role {
        ObjectRole = Qt::UserRole + 1,  ///< The id of the object, as qulonglong.
        StatusRole,     ///< The worst status under the row, as int (form::Severity).
        OwnStatusRole,  ///< The status of the object itself, as int.
        SearchRole,     ///< The text the filter searches: title and type title.
    };
    enum Column { title_column = 0, status_column = 1 };

    explicit ModelTreeModel(Workbench& workbench, QObject* parent = nullptr);
    ~ModelTreeModel() override;
    ModelTreeModel(const ModelTreeModel&) = delete;
    ModelTreeModel& operator=(const ModelTreeModel&) = delete;
    ModelTreeModel(ModelTreeModel&&) = delete;
    ModelTreeModel& operator=(ModelTreeModel&&) = delete;

    /// Called with a message for the user when renaming fails.
    void set_report(std::function<void(const QString&)> report) { report_ = std::move(report); }

    /// The row of an object (column 0); invalid if it is not in the tree.
    [[nodiscard]] QModelIndex index_of(model::ObjectId id) const;
    [[nodiscard]] model::ObjectId object_at(const QModelIndex& index) const;
    /// The problems of an object as the tree shows them.
    [[nodiscard]] const std::vector<form::Problem>& problems(model::ObjectId id) const;
    /// The forms of the enabled modules, as the tree checks objects against them.
    [[nodiscard]] const form::FormIndex& forms() const noexcept { return forms_; }
    /// Builds the tree again.
    void reload();

    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

Q_SIGNALS:
    /// The forms were read again (modules were enabled or disabled).
    void forms_changed();

private:
    struct Node {
        model::ObjectId id;
        Node* parent = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
        std::vector<form::Problem> problems;
        form::Severity own = form::Severity::ok;
        form::Severity worst = form::Severity::ok;
    };

    void build(Node& node, std::span<const model::ObjectId> children);
    void check(Node& node);
    /// Recomputes the worst status of `node` and its ancestors, and reports the rows that changed.
    void update_worst(Node* node);
    void on_model_changed(const command::ModelChange& change);
    [[nodiscard]] Node* node(const QModelIndex& index) const;
    [[nodiscard]] QModelIndex index_of(const Node& node, int column) const;
    [[nodiscard]] QString tooltip(const Node& node) const;

    Workbench& workbench_;
    form::FormIndex forms_;
    Node root_;
    std::map<model::ObjectId, Node*> nodes_;
    mutable std::map<std::pair<std::string, std::string>, QIcon> icons_;
    std::function<void(const QString&)> report_;
    Subscription model_changed_;
    Subscription extension_events_;
    Subscription language_;
};

/// The zone "Model Tree": a search field over the tree of the model, the context menu of a node
/// with the items of modules (`contextMenus`, `location = "tree"`) and of the shell (rename,
/// delete), and the selection, which sets the keys of the tree and the object commands take.
///
/// Object names: the search field "treeFilter", the tree "modelTreeView", actions "tree.rename"
/// and "tree.delete".
class ModelTreePanel : public QWidget {
    Q_OBJECT

public:
    /// The menu of the items of modules at "tree" that show now, or nullptr; the caller owns it.
    using ModuleMenu = std::function<QMenu*(QWidget* parent)>;

    ModelTreePanel(Workbench& workbench, Commands& commands, QWidget* parent = nullptr);
    ~ModelTreePanel() override;
    ModelTreePanel(const ModelTreePanel&) = delete;
    ModelTreePanel& operator=(const ModelTreePanel&) = delete;
    ModelTreePanel(ModelTreePanel&&) = delete;
    ModelTreePanel& operator=(ModelTreePanel&&) = delete;

    /// Where the items of modules for the context menu come from (set once the registry is built
    /// into the window).
    void set_module_menu(ModuleMenu menu) { module_menu_ = std::move(menu); }
    /// Called after the selection changed, to refresh what depends on it.
    void set_on_selection(std::function<void()> listener) { on_selection_ = std::move(listener); }

    [[nodiscard]] ModelTreeModel& model() const noexcept { return *model_; }
    [[nodiscard]] QSortFilterProxyModel& proxy() const noexcept { return *proxy_; }
    [[nodiscard]] QTreeView* view() const noexcept { return view_; }
    [[nodiscard]] QLineEdit* filter() const noexcept { return filter_; }
    [[nodiscard]] QAction* rename_action() const noexcept { return rename_; }
    [[nodiscard]] QAction* delete_action() const noexcept { return delete_; }

    /// The selected object; invalid if none.
    [[nodiscard]] model::ObjectId selected() const noexcept { return selected_; }
    /// Selects an object (and shows it, expanding its parents); an invalid id clears the selection.
    void select(model::ObjectId id);
    /// The context menu for the selection: items of modules, then rename and delete; nullptr if
    /// nothing shows. The caller owns it.
    [[nodiscard]] QMenu* context_menu(QWidget* parent = nullptr);

Q_SIGNALS:
    /// The selected object changed.
    void selection_changed();

private:
    void on_current_changed();
    void set_selected(model::ObjectId id);
    void remove_selected();
    void retranslate();
    [[nodiscard]] QModelIndex view_index(model::ObjectId id) const;
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    Workbench& workbench_;
    Commands& commands_;
    ModelTreeModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeView* view_ = nullptr;
    QAction* rename_ = nullptr;
    QAction* delete_ = nullptr;
    model::ObjectId selected_;
    ModuleMenu module_menu_;
    std::function<void()> on_selection_;
    /// Kept over a reset of the model: the expanded objects.
    std::set<model::ObjectId> expanded_;
    Subscription language_;
};

}  // namespace fem::shell
