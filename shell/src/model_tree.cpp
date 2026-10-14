#include "fem/shell/model_tree.h"

#include "fem/core/command/model_commands.h"
#include "fem/core/log/log.h"
#include "fem/core/model/object.h"
#include "fem/shell/commands.h"
#include "fem/shell/icons.h"
#include "fem/shell/strings.h"

#include <QAction>
#include <QHeaderView>
#include <QIconEngine>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>
#include <functional>
#include <utility>

namespace fem::shell {

namespace {

/// An icon drawn the same whether its node is expanded or not: views draw the icons of expanded
/// nodes in the "on" state, which tinted icons show in the accent colour.
class StatelessIconEngine final : public QIconEngine {
public:
    explicit StatelessIconEngine(QIcon icon) : icon_(std::move(icon)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State /*state*/) override {
        icon_.paint(painter, rect, Qt::AlignCenter, mode, QIcon::Off);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State /*state*/) override {
        return icon_.pixmap(size, mode, QIcon::Off);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State /*state*/,
                         qreal scale) override {
        return icon_.pixmap(size / scale, scale, mode, QIcon::Off);
    }
    [[nodiscard]] QIconEngine* clone() const override { return new StatelessIconEngine(icon_); }
    [[nodiscard]] QString key() const override { return QStringLiteral("StressLabStateless"); }
    bool isNull() override { return icon_.isNull(); }

private:
    QIcon icon_;
};

/// The name of a module for users; the id if it is not installed.
std::string module_name(const Workbench& workbench, const std::string& owner) {
    if (owner == model::core_owner) {
        return workbench.localization.text(strings_owner, "tree.core");
    }
    const extension::ExtensionInfo* info = workbench.extensions.find(owner);
    return info != nullptr && info->manifest
               ? workbench.localization.manifest_text(owner, info->manifest->name)
               : owner;
}

const extension::TreeNodeTypeContribution* tree_node_type(const Workbench& workbench,
                                                          std::string_view owner,
                                                          std::string_view type) {
    for (const extension::TreeNodeTypeContribution* entry :
         workbench.extensions.registry().tree_node_types()) {
        if (entry->owner == owner && entry->type == type) {
            return entry;
        }
    }
    return nullptr;
}

}  // namespace

void declare_tree_keys(context::ContextKeys& keys) {
    for (const std::string_view name : {selected_node_type_key, selected_node_owner_key}) {
        (void)keys.declare({.name = std::string(name),
                            .owner = std::string(strings_owner),
                            .type = context::ContextValue::Kind::string,
                            .initial = context::ContextValue(std::string())});
    }
}

QString type_title(const Workbench& workbench, std::string_view owner, std::string_view type) {
    if (const extension::TreeNodeTypeContribution* entry = tree_node_type(workbench, owner, type)) {
        return to_qstring(workbench.localization.manifest_text(owner, entry->title));
    }
    if (owner == model::core_owner) {
        if (const auto found =
                workbench.localization.find(strings_owner, "tree.type." + std::string(type))) {
            return to_qstring(*found);
        }
    }
    return to_qstring(type);
}

QIcon type_icon(const Workbench& workbench, std::string_view owner, std::string_view type) {
    const extension::TreeNodeTypeContribution* entry = tree_node_type(workbench, owner, type);
    return module_icon(workbench.extensions, owner, entry != nullptr ? entry->icon : "", true);
}

QString object_title(const Workbench& workbench, const model::Object& object) {
    return object.name().empty() ? type_title(workbench, object.owner(), object.type())
                                 : to_qstring(object.name());
}

// --- ModelTreeModel ------------------------------------------------------------------------------

ModelTreeModel::ModelTreeModel(Workbench& workbench, QObject* parent)
    : QAbstractItemModel(parent), workbench_(workbench), forms_(workbench.extensions.registry()) {
    reload();
    model_changed_ = workbench_.bus.on_model_changed(
        [this](const command::ModelChange& change) { on_model_changed(change); });
    extension_events_ =
        workbench_.extensions.on_event([this](const extension::ExtensionEvent& event) {
            using Kind = extension::ExtensionEvent::Kind;
            if (event.kind == Kind::enabled || event.kind == Kind::deactivated) {
                forms_ = form::FormIndex(workbench_.extensions.registry());
                reload();
                Q_EMIT forms_changed();
            }
        });
    language_ = workbench_.localization.on_language_changed([this](const std::string&) {
        icons_.clear();
        reload();
    });
}

ModelTreeModel::~ModelTreeModel() = default;

void ModelTreeModel::reload() {
    beginResetModel();
    root_.children.clear();
    nodes_.clear();
    build(root_, workbench_.bus.document().roots());
    endResetModel();
}

void ModelTreeModel::build(Node& parent, std::span<const model::ObjectId> children) {
    const model::Document& document = workbench_.bus.document();
    for (const model::ObjectId id : children) {
        const model::Object& object = document.get(id);
        if (document.is_opaque(object)) {
            continue;  // Hidden with its subtree until its module is enabled.
        }
        auto node = std::make_unique<Node>();
        node->id = id;
        node->parent = &parent;
        node->row = static_cast<int>(parent.children.size());
        nodes_[id] = node.get();
        check(*node);
        build(*node, object.children());
        node->worst = node->own;
        for (const auto& child : node->children) {
            node->worst = std::max(node->worst, child->worst);
        }
        parent.children.push_back(std::move(node));
    }
}

void ModelTreeModel::check(Node& node) {
    node.problems = form::object_problems(workbench_.bus.document(), forms_, node.id);
    node.own = form::worst(node.problems);
}

void ModelTreeModel::update_worst(Node* node) {
    for (Node* current = node; current != nullptr && current != &root_; current = current->parent) {
        form::Severity worst = current->own;
        for (const auto& child : current->children) {
            worst = std::max(worst, child->worst);
        }
        const bool changed = worst != current->worst;
        current->worst = worst;
        if (current == node || changed) {
            Q_EMIT dataChanged(index_of(*current, title_column), index_of(*current, status_column));
        } else {
            break;
        }
    }
}

void ModelTreeModel::on_model_changed(const command::ModelChange& change) {
    const model::ChangeSummary& summary = change.summary;
    if (!summary.created.empty() || !summary.removed.empty()) {
        reload();
        return;
    }
    for (const model::ObjectId id : summary.updated) {
        const auto found = nodes_.find(id);
        if (found == nodes_.end()) {
            continue;
        }
        check(*found->second);
        update_worst(found->second);
    }
}

QModelIndex ModelTreeModel::index_of(model::ObjectId id) const {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? QModelIndex() : index_of(*found->second, title_column);
}

QModelIndex ModelTreeModel::index_of(const Node& node, int column) const {
    return createIndex(node.row, column, const_cast<Node*>(&node));
}

model::ObjectId ModelTreeModel::object_at(const QModelIndex& index) const {
    const Node* found = node(index);
    return found != nullptr ? found->id : model::ObjectId();
}

const std::vector<form::Problem>& ModelTreeModel::problems(model::ObjectId id) const {
    static const std::vector<form::Problem> none;
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? none : found->second->problems;
}

ModelTreeModel::Node* ModelTreeModel::node(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != this) {
        return nullptr;
    }
    return static_cast<Node*>(index.internalPointer());
}

QModelIndex ModelTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (column < 0 || column > status_column || row < 0) {
        return {};
    }
    const Node* owner = parent.isValid() ? node(parent) : &root_;
    if (owner == nullptr || row >= static_cast<int>(owner->children.size())) {
        return {};
    }
    return index_of(*owner->children[static_cast<std::size_t>(row)], column);
}

QModelIndex ModelTreeModel::parent(const QModelIndex& child) const {
    const Node* found = node(child);
    if (found == nullptr || found->parent == nullptr || found->parent == &root_) {
        return {};
    }
    return index_of(*found->parent, title_column);
}

int ModelTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() && parent.column() != title_column) {
        return 0;
    }
    const Node* owner = parent.isValid() ? node(parent) : &root_;
    return owner != nullptr ? static_cast<int>(owner->children.size()) : 0;
}

int ModelTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return status_column + 1;
}

QVariant ModelTreeModel::data(const QModelIndex& index, int role) const {
    const Node* found = node(index);
    if (found == nullptr) {
        return {};
    }
    const model::Object& object = workbench_.bus.document().get(found->id);
    switch (role) {
        case ObjectRole:
            return QVariant::fromValue<qulonglong>(found->id.value());
        case StatusRole:
            return static_cast<int>(found->worst);
        case OwnStatusRole:
            return static_cast<int>(found->own);
        case SearchRole:
            return object_title(workbench_, object) + QLatin1Char('\n') +
                   type_title(workbench_, object.owner(), object.type());
        case Qt::ToolTipRole:
            return tooltip(*found);
        default:
            break;
    }
    if (index.column() == status_column) {
        if (role == Qt::DecorationRole) {
            return status_icon(found->worst);
        }
        return {};
    }
    switch (role) {
        case Qt::DisplayRole:
            return object_title(workbench_, object);
        case Qt::EditRole:
            return to_qstring(object.name());
        case Qt::DecorationRole: {
            const auto key = std::pair{object.owner(), object.type()};
            auto cached = icons_.find(key);
            if (cached == icons_.end()) {
                cached = icons_
                             .emplace(key, QIcon(new StatelessIconEngine(type_icon(
                                               workbench_, object.owner(), object.type()))))
                             .first;
            }
            return cached->second;
        }
        default:
            break;
    }
    return {};
}

Qt::ItemFlags ModelTreeModel::flags(const QModelIndex& index) const {
    if (node(index) == nullptr) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (index.column() == title_column) {
        flags |= Qt::ItemIsEditable;
    }
    return flags;
}

bool ModelTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const Node* found = node(index);
    if (found == nullptr || role != Qt::EditRole || index.column() != title_column) {
        return false;
    }
    const std::string name = value.toString().trimmed().toStdString();
    const model::Object& object = workbench_.bus.document().get(found->id);
    if (name == object.name()) {
        return false;
    }
    const model::ObjectId id = found->id;
    try {
        (void)workbench_.bus.execute(command::model_commands::rename,
                                     {{"object", command::Value(id)}, {"name", name}});
        return true;
    } catch (const std::exception& error) {
        log::logger("shell")->warn("renaming {} failed: {}", model::to_string(id), error.what());
        if (report_) {
            report_(text(workbench_.localization, "tree.renameFailed",
                         {{"name", name}, {"message", std::string(error.what())}}));
        }
        return false;
    }
}

QString ModelTreeModel::tooltip(const Node& node) const {
    const model::Object& object = workbench_.bus.document().get(node.id);
    QStringList lines;
    lines.push_back(
        text(workbench_.localization, "tree.typeOf",
             {{"type", type_title(workbench_, object.owner(), object.type()).toStdString()},
              {"module", module_name(workbench_, object.owner())}}));
    for (const form::Problem& problem : node.problems) {
        lines.push_back(to_qstring(form::text(problem, workbench_.localization, object.owner())));
    }
    if (node.worst > node.own) {
        lines.push_back(text(workbench_.localization, "tree.problemsInside"));
    }
    return lines.join(QLatin1Char('\n'));
}

// --- ModelTreePanel ------------------------------------------------------------------------------

ModelTreePanel::ModelTreePanel(Workbench& workbench, Commands& commands, QWidget* parent)
    : QWidget(parent), workbench_(workbench), commands_(commands) {
    declare_tree_keys(workbench_.keys);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("treeFilter"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    model_ = new ModelTreeModel(workbench_, this);
    model_->set_report([this](const QString& message) { commands_.report(message); });
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setRecursiveFilteringEnabled(true);
    proxy_->setAutoAcceptChildRows(true);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterRole(ModelTreeModel::SearchRole);
    proxy_->setFilterKeyColumn(ModelTreeModel::title_column);

    view_ = new QTreeView(this);
    view_->setObjectName(QStringLiteral("modelTreeView"));
    view_->setModel(proxy_);
    view_->setHeaderHidden(true);
    view_->setUniformRowHeights(true);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(ModelTreeModel::title_column, QHeaderView::Stretch);
    view_->header()->setSectionResizeMode(ModelTreeModel::status_column,
                                          QHeaderView::ResizeToContents);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(view_, 1);

    rename_ = new QAction(this);
    rename_->setObjectName(QStringLiteral("tree.rename"));
    rename_->setShortcut(QKeySequence(Qt::Key_F2));
    rename_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(rename_, &QAction::triggered, this, [this] {
        const QModelIndex current = view_->currentIndex();
        if (current.isValid()) {
            view_->edit(current.siblingAtColumn(ModelTreeModel::title_column));
        }
    });
    delete_ = new QAction(this);
    delete_->setObjectName(QStringLiteral("tree.delete"));
    delete_->setShortcut(QKeySequence::Delete);
    delete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(delete_, &QAction::triggered, this, &ModelTreePanel::remove_selected);
    view_->addAction(rename_);
    view_->addAction(delete_);

    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        proxy_->setFilterFixedString(text);
        if (!text.isEmpty()) {
            view_->expandAll();
        }
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &ModelTreePanel::on_current_changed);
    connect(view_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QModelIndex at = view_->indexAt(position);
        if (at.isValid()) {
            view_->setCurrentIndex(at);
        } else {
            view_->setCurrentIndex({});
            view_->clearSelection();
        }
        QMenu* menu = context_menu(this);
        if (menu != nullptr) {
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(view_->viewport()->mapToGlobal(position));
        }
    });
    // A reset (objects created or removed, modules enabled or disabled) keeps what was expanded and
    // selected, as far as it is still there.
    connect(model_, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        expanded_.clear();
        const std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parent) {
            for (int row = 0; row < proxy_->rowCount(parent); ++row) {
                const QModelIndex child = proxy_->index(row, 0, parent);
                if (view_->isExpanded(child)) {
                    expanded_.insert(
                        model::ObjectId(child.data(ModelTreeModel::ObjectRole).toULongLong()));
                }
                visit(child);
            }
        };
        visit({});
    });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] {
        for (const model::ObjectId id : expanded_) {
            if (const QModelIndex index = view_index(id); index.isValid()) {
                view_->setExpanded(index, true);
            }
        }
        if (!filter_->text().isEmpty()) {
            view_->expandAll();
        }
        const model::ObjectId keep = selected_;
        if (keep.is_valid() && model_->index_of(keep).isValid()) {
            select(keep);
        } else {
            set_selected({});
        }
    });

    retranslate();
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
}

ModelTreePanel::~ModelTreePanel() {
    // The view and the model are deleted after the members of this class: what they report then
    // (the current row going away) must not reach it.
    for (QObject* child : findChildren<QObject*>()) {
        QObject::disconnect(child, nullptr, this, nullptr);
    }
    // The selection goes with the panel: nothing selected is left in the keys and commands.
    commands_.set_selected_object({});
}

void ModelTreePanel::select(model::ObjectId id) {
    const QModelIndex index = view_index(id);
    if (!index.isValid()) {
        view_->setCurrentIndex({});
        view_->clearSelection();
        set_selected({});
        return;
    }
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        view_->setExpanded(parent, true);
    }
    view_->setCurrentIndex(index);
    view_->scrollTo(index);
    set_selected(id);
}

QMenu* ModelTreePanel::context_menu(QWidget* parent) {
    QMenu* menu = module_menu_ ? module_menu_(parent) : nullptr;
    if (!selected_.is_valid()) {
        return menu;
    }
    if (menu == nullptr) {
        menu = new QMenu(parent);
    } else {
        menu->addSeparator();
    }
    menu->addAction(rename_);
    menu->addAction(delete_);
    return menu;
}

void ModelTreePanel::on_current_changed() {
    const QModelIndex current = proxy_->mapToSource(view_->currentIndex());
    set_selected(model_->object_at(current));
}

void ModelTreePanel::set_selected(model::ObjectId id) {
    const bool changed = id != selected_;
    selected_ = id;
    commands_.set_selected_object(id);
    std::string type;
    std::string owner;
    if (const model::Object* object = workbench_.bus.document().find(id)) {
        type = object->type();
        owner = object->owner();
    }
    workbench_.keys.set({{std::string(selected_node_type_key), context::ContextValue(type)},
                         {std::string(selected_node_owner_key), context::ContextValue(owner)}});
    rename_->setEnabled(id.is_valid());
    delete_->setEnabled(id.is_valid());
    if (on_selection_) {
        on_selection_();
    }
    if (changed) {
        Q_EMIT selection_changed();
    }
}

void ModelTreePanel::remove_selected() {
    const model::ObjectId id = selected_;
    const model::Document& document = workbench_.bus.document();
    const model::Object* object = document.find(id);
    if (object == nullptr) {
        return;
    }
    const std::vector<model::RemovalBlocker> blockers = document.removal_blockers(id);
    if (!blockers.empty()) {
        const model::Object& opaque = document.get(blockers.front().opaque);
        commands_.report(
            text("tree.removeBlocked", {{"name", object_title(workbench_, *object).toStdString()},
                                        {"module", module_name(workbench_, opaque.owner())}}));
        return;
    }
    try {
        (void)workbench_.bus.execute(command::model_commands::remove,
                                     {{"object", command::Value(id)}});
    } catch (const std::exception& error) {
        log::logger("shell")->warn("removing {} failed: {}", model::to_string(id), error.what());
        commands_.report(
            text("tree.removeFailed", {{"name", object_title(workbench_, *object).toStdString()},
                                       {"message", std::string(error.what())}}));
    }
}

void ModelTreePanel::retranslate() {
    filter_->setPlaceholderText(text("tree.filter"));
    rename_->setText(text("tree.rename"));
    delete_->setText(text("tree.delete"));
}

QModelIndex ModelTreePanel::view_index(model::ObjectId id) const {
    return proxy_->mapFromSource(model_->index_of(id));
}

QString ModelTreePanel::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

}  // namespace fem::shell
