#include "fem/shell/extensions_panel.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/profile.h"
#include "fem/core/log/log.h"
#include "fem/shell/keys.h"
#include "fem/shell/strings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <set>
#include <system_error>

namespace fem::shell {

namespace {

using extension::ExtensionInfo;
using extension::ExtensionSource;

QString qid(std::string_view id) {
    return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

QLabel* make_label(const QString& text, QWidget* parent, bool rich = false) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(rich ? Qt::RichText : Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel* make_heading(const QString& text, QWidget* parent) {
    QLabel* label = make_label(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

/// A label in the muted text colour.
QLabel* make_muted(const QString& text, QWidget* parent) {
    QLabel* label = make_label(text, parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

std::string join(const std::vector<std::string>& parts) {
    std::string text;
    for (const std::string& part : parts) {
        if (!text.empty()) {
            text += ", ";
        }
        text += part;
    }
    return text;
}

}  // namespace

// --- What a module adds --------------------------------------------------------------------------

std::vector<ContributionLine> contribution_lines(const ExtensionInfo& extension,
                                                 const l10n::Localization& localization) {
    const extension::Contributions& c = extension.manifest->contributes;
    const std::string& owner = extension.id();
    const auto title = [&](const std::string& text) {
        return to_qstring(localization.manifest_text(owner, text));
    };
    const auto command_title = [&](const std::string& id) {
        for (const extension::CommandContribution& command : c.commands) {
            if (command.id == id) {
                return title(command.title);
            }
        }
        return qid(id);
    };
    std::vector<ContributionLine> lines;
    const auto add = [&](std::string_view key, QStringList items) {
        items.removeDuplicates();
        if (!items.isEmpty()) {
            lines.push_back({text(localization, key), std::move(items)});
        }
    };

    QStringList items;
    for (const extension::StageContribution& stage : c.stages) {
        items.push_back(title(stage.title));
    }
    add("contributes.stages", std::exchange(items, {}));
    for (const extension::TreeNodeTypeContribution& node : c.tree_node_types) {
        items.push_back(title(node.title));
    }
    add("contributes.treeNodeTypes", std::exchange(items, {}));
    for (const extension::ContextToolbarContribution& item : c.context_toolbar) {
        items.push_back(command_title(item.command));
    }
    add("contributes.contextToolbar", std::exchange(items, {}));
    for (const extension::PropertySchemaContribution& schema : c.property_schemas) {
        QString name = qid(schema.type);
        for (const extension::TreeNodeTypeContribution& node : c.tree_node_types) {
            if (node.type == schema.type) {
                name = title(node.title);
            }
        }
        items.push_back(name);
    }
    add("contributes.propertySchemas", std::exchange(items, {}));
    for (const extension::CommandContribution& command : c.commands) {
        items.push_back(title(command.title));
    }
    add("contributes.commands", std::exchange(items, {}));
    for (const extension::MenuContribution& menu : c.menus) {
        items.push_back(command_title(menu.command));
    }
    for (const extension::MenuContribution& menu : c.context_menus) {
        items.push_back(command_title(menu.command));
    }
    add("contributes.menus", std::exchange(items, {}));
    for (const extension::FileFormatContribution& format : c.file_formats) {
        QString extensions;
        for (const std::string& suffix : format.extensions) {
            extensions += (extensions.isEmpty() ? QString() : QStringLiteral(", ")) + qid(suffix);
        }
        const QString name = format.title.empty() ? qid(format.id) : title(format.title);
        items.push_back(extensions.isEmpty()
                            ? name
                            : name + QStringLiteral(" (") + extensions + QLatin1Char(')'));
    }
    add("contributes.fileFormats", std::exchange(items, {}));
    for (const extension::SolverContribution& solver : c.solvers) {
        items.push_back(title(solver.label));
    }
    add("contributes.solvers", std::exchange(items, {}));
    for (const extension::MesherContribution& mesher : c.meshers) {
        items.push_back(title(mesher.label));
    }
    add("contributes.meshers", std::exchange(items, {}));
    for (const extension::ActivityBarContribution& activity : c.activity_bar) {
        items.push_back(title(activity.title));
    }
    add("contributes.activityBar", std::exchange(items, {}));
    for (const extension::BottomPanelTabContribution& tab : c.bottom_panel_tabs) {
        items.push_back(title(tab.title));
    }
    add("contributes.bottomPanelTabs", std::exchange(items, {}));
    for (const extension::ViewportLayerContribution& layer : c.viewport_layers) {
        items.push_back(title(layer.title));
    }
    for (const extension::ResultFilterContribution& filter : c.result_filters) {
        items.push_back(title(filter.title));
    }
    for (const extension::ResultIndicatorContribution& indicator : c.result_indicators) {
        items.push_back(title(indicator.title));
    }
    add("contributes.viewport", std::exchange(items, {}));
    for (const extension::SettingContribution& setting : c.settings) {
        const auto it = setting.schema.find("title");
        items.push_back(it != setting.schema.end() && it->is_string()
                            ? title(it->get<std::string>())
                            : qid(setting.key));
    }
    add("contributes.settings", std::exchange(items, {}));
    for (const extension::KeybindingContribution& binding : c.keybindings) {
        items.push_back(key_sequence(binding.key).toString(QKeySequence::NativeText) +
                        QStringLiteral(" — ") + command_title(binding.command));
    }
    add("contributes.keybindings", std::exchange(items, {}));
    return lines;
}

// --- ExtensionsPanel -----------------------------------------------------------------------------

ExtensionsPanel::ExtensionsPanel(Workbench& workbench, QWidget* parent)
    : QWidget(parent), workbench_(workbench) {
    setObjectName(QStringLiteral("extensionsPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    status_ = make_label(QString(), this);
    status_->setObjectName(QStringLiteral("extensions.status"));
    status_->hide();
    layout->addWidget(status_);
    pages_ = new QStackedWidget(this);
    layout->addWidget(pages_, 1);
    build_list_page();
    build_card_page();

    events_ = workbench_.extensions.on_event([this](const extension::ExtensionEvent& event) {
        using Kind = extension::ExtensionEvent::Kind;
        if (event.kind == Kind::uninstalled && event.id == card_) {
            // Its info is gone: never show it again.
            card_.clear();
        }
        schedule_rebuild();
    });
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
    retranslate();
}

ExtensionsPanel::~ExtensionsPanel() {
    // The search field may lose the focus while it is destroyed (Linux) and report to `this`.
    search_->disconnect(this);
}

void ExtensionsPanel::build_list_page() {
    auto* page = new QWidget(pages_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    search_ = new QLineEdit(page);
    search_->setObjectName(QStringLiteral("extensions.search"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, [this] { rebuild_installed(); });
    layout->addWidget(search_);

    install_ = new QPushButton(page);
    install_->setObjectName(QStringLiteral("extensions.installFolder"));
    connect(install_, &QPushButton::clicked, this, &ExtensionsPanel::install_from_folder);
    layout->addWidget(install_);

    tabs_ = new QTabWidget(page);
    tabs_->setObjectName(QStringLiteral("extensions.tabs"));
    tabs_->setDocumentMode(true);
    installed_ = new QListWidget(tabs_);
    installed_->setObjectName(QStringLiteral("extensions.installed"));
    installed_->setSelectionMode(QAbstractItemView::NoSelection);
    connect(installed_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        show_card(item->data(Qt::UserRole).toString().toStdString());
    });
    catalog_ = make_muted(QString(), tabs_);
    catalog_->setObjectName(QStringLiteral("extensions.catalog"));
    catalog_->setAlignment(Qt::AlignCenter);
    auto* updates_page = new QWidget(tabs_);
    auto* updates_layout = new QVBoxLayout(updates_page);
    updates_layout->setContentsMargins(0, 0, 0, 0);
    updates_ = new QListWidget(updates_page);
    updates_->setObjectName(QStringLiteral("extensions.updates"));
    updates_->setSelectionMode(QAbstractItemView::NoSelection);
    no_updates_ = make_muted(QString(), updates_page);
    no_updates_->setAlignment(Qt::AlignCenter);
    updates_layout->addWidget(updates_, 1);
    updates_layout->addWidget(no_updates_, 1);
    tabs_->addTab(installed_, QString());
    tabs_->addTab(catalog_, QString());
    tabs_->addTab(updates_page, QString());
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == static_cast<int>(Tab::updates)) {
            rebuild_updates();  // Folders may have changed meanwhile.
        }
    });
    layout->addWidget(tabs_, 1);
    build_profiles(layout);
    pages_->addWidget(page);
}

void ExtensionsPanel::build_profiles(QVBoxLayout* layout) {
    profiles_label_ = make_heading(QString(), this);
    layout->addWidget(profiles_label_);
    auto* row = new QHBoxLayout;
    profiles_box_ = new QComboBox(this);
    profiles_box_->setObjectName(QStringLiteral("profiles.box"));
    profiles_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profiles_box_->setMinimumContentsLength(8);
    connect(profiles_box_, &QComboBox::currentIndexChanged, this,
            [this] { apply_->setEnabled(!selected_profile().empty()); });
    apply_ = new QPushButton(this);
    apply_->setObjectName(QStringLiteral("profiles.apply"));
    connect(apply_, &QPushButton::clicked, this, [this] {
        if (const std::string name = selected_profile(); !name.empty()) {
            apply_profile(name);
        }
    });
    more_ = new QToolButton(this);
    more_->setObjectName(QStringLiteral("profiles.more"));
    more_->setText(QStringLiteral("⋯"));
    more_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more_);
    const auto add = [&](const char* name, auto slot) {
        QAction* action = menu->addAction(QString(), this, slot);
        action->setObjectName(QString::fromLatin1(name));
        return action;
    };
    add("profiles.saveAs", [this] { (void)ask_profile_name(); });
    add("profiles.delete", [this] {
        if (const std::string name = selected_profile(); !name.empty()) {
            delete_profile(name);
        }
    });
    menu->addSeparator();
    add("profiles.import", [this] { import_profile(); });
    add("profiles.export", [this] {
        if (const std::string name = selected_profile(); !name.empty()) {
            export_profile(name);
        }
    });
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        const bool selected = !selected_profile().empty();
        for (QAction* action : menu->actions()) {
            const QString name = action->objectName();
            if (name == QLatin1String("profiles.delete") ||
                name == QLatin1String("profiles.export")) {
                action->setEnabled(selected);
            }
        }
    });
    more_->setMenu(menu);
    row->addWidget(profiles_box_, 1);
    row->addWidget(apply_);
    row->addWidget(more_);
    layout->addLayout(row);
    const bool available = workbench_.profiles != nullptr;
    profiles_label_->setVisible(available);
    profiles_box_->setVisible(available);
    apply_->setVisible(available);
    more_->setVisible(available);
}

void ExtensionsPanel::build_card_page() {
    card_page_ = new QWidget(pages_);
    card_page_->setObjectName(QStringLiteral("extensions.card"));
    auto* layout = new QVBoxLayout(card_page_);
    layout->setContentsMargins(0, 0, 0, 0);
    back_ = new QPushButton(card_page_);
    back_->setObjectName(QStringLiteral("card.back"));
    back_->setFlat(true);
    connect(back_, &QPushButton::clicked, this, &ExtensionsPanel::show_list);
    auto* top = new QHBoxLayout;
    top->addWidget(back_);
    top->addStretch(1);
    layout->addLayout(top);
    auto* scroll = new QScrollArea(card_page_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // The card is as wide as the panel: its texts wrap.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    card_content_ = new QWidget(scroll);
    card_layout_ = new QVBoxLayout(card_content_);
    card_layout_->setContentsMargins(4, 4, 4, 4);
    scroll->setWidget(card_content_);
    layout->addWidget(scroll, 1);
    pages_->addWidget(card_page_);
}

void ExtensionsPanel::schedule_rebuild() {
    if (rebuild_pending_) {
        return;
    }
    rebuild_pending_ = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (rebuild_pending_) {
                rebuild();
            }
        },
        Qt::QueuedConnection);
}

void ExtensionsPanel::rebuild() {
    rebuild_pending_ = false;
    rebuild_installed();
    rebuild_updates();
    rebuild_profiles();
    if (!card_.empty() && workbench_.extensions.find(card_) == nullptr) {
        card_.clear();
    }
    if (card_.empty()) {
        pages_->setCurrentIndex(0);
    } else {
        rebuild_card();
    }
    Q_EMIT rebuilt();
}

void ExtensionsPanel::rebuild_installed() {
    installed_->clear();
    listed_.clear();
    const QString filter = search_->text().trimmed();
    std::vector<const ExtensionInfo*> modules = workbench_.extensions.extensions();
    std::stable_sort(modules.begin(), modules.end(),
                     [this](const ExtensionInfo* a, const ExtensionInfo* b) {
                         return module_name(*a).compare(module_name(*b), Qt::CaseInsensitive) < 0;
                     });
    for (const ExtensionInfo* info : modules) {
        const QString name = module_name(*info);
        const QString description = to_qstring(
            workbench_.localization.manifest_text(info->id(), info->manifest->description));
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) &&
            !qid(info->id()).contains(filter, Qt::CaseInsensitive) &&
            !description.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        listed_.push_back(info->id());
        auto* row = new QWidget(installed_);
        row->setObjectName(QStringLiteral("row:") + qid(info->id()));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(4, 4, 4, 4);
        auto* toggle = new QCheckBox(row);
        toggle->setObjectName(QStringLiteral("toggle:") + qid(info->id()));
        toggle->setChecked(info->enabled());
        toggle->setEnabled(info->compatible());
        toggle->setToolTip(info->enabled() ? tr_text("extensions.disable")
                                           : tr_text("extensions.enable"));
        connect(toggle, &QCheckBox::clicked, this,
                [this, id = info->id()](bool on) { set_enabled(id, on); });
        line->addWidget(toggle, 0, Qt::AlignVCenter);
        auto* texts = new QVBoxLayout;
        texts->setSpacing(1);
        QString title =
            name + QStringLiteral("  ") + to_qstring(info->manifest->version.to_string());
        if (info->source == ExtensionSource::local) {
            title += QStringLiteral("  · ") + tr_text("extensions.local");
        }
        texts->addWidget(make_heading(title, row));
        QString detail = description;
        if (!info->compatible()) {
            detail = tr_text("extensions.incompatible", {{"reason", info->incompatibility}});
        } else if (!info->activation_error.empty()) {
            detail = tr_text("extensions.failed", {{"reason", info->activation_error}});
        }
        if (!detail.isEmpty()) {
            texts->addWidget(make_muted(detail, row));
        }
        line->addLayout(texts, 1);
        auto* item = new QListWidgetItem(installed_);
        item->setData(Qt::UserRole, qid(info->id()));
        item->setSizeHint(row->sizeHint());
        installed_->setItemWidget(item, row);
    }
}

void ExtensionsPanel::rebuild_updates() {
    updates_->clear();
    updates_offered_.clear();
    for (const extension::ExtensionUpdate& offer : workbench_.extensions.updates()) {
        const ExtensionInfo* info = workbench_.extensions.find(offer.id);
        if (info == nullptr) {
            continue;
        }
        updates_offered_.push_back(offer.id);
        auto* row = new QWidget(updates_);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(4, 4, 4, 4);
        line->addWidget(make_label(tr_text("extensions.updateRow",
                                           {{"module", module_name(*info).toStdString()},
                                            {"installed", offer.installed.to_string()},
                                            {"available", offer.available.to_string()}}),
                                   row),
                        1);
        auto* button = new QPushButton(tr_text("extensions.update"), row);
        button->setObjectName(QStringLiteral("update:") + qid(offer.id));
        connect(button, &QPushButton::clicked, this, [this, id = offer.id] { update_module(id); });
        line->addWidget(button);
        auto* item = new QListWidgetItem(updates_);
        item->setSizeHint(row->sizeHint());
        updates_->setItemWidget(item, row);
    }
    updates_->setVisible(!updates_offered_.empty());
    no_updates_->setVisible(updates_offered_.empty());
    tabs_->setTabText(
        static_cast<int>(Tab::updates),
        updates_offered_.empty()
            ? tr_text("extensions.tabUpdates")
            : tr_text("extensions.tabUpdatesCount",
                      {{"count", static_cast<std::int64_t>(updates_offered_.size())}}));
}

void ExtensionsPanel::rebuild_profiles() {
    if (workbench_.profiles == nullptr) {
        return;
    }
    const std::string selected = selected_profile();
    const QSignalBlocker blocker(profiles_box_);
    profiles_box_->clear();
    profiles_box_->addItem(tr_text("profiles.choose"), QString());
    for (const std::string& name : workbench_.profiles->names()) {
        profiles_box_->addItem(to_qstring(name), to_qstring(name));
    }
    const int index = profiles_box_->findData(to_qstring(selected));
    profiles_box_->setCurrentIndex(index < 0 ? 0 : index);
    apply_->setEnabled(!selected_profile().empty());
}

void ExtensionsPanel::rebuild_card() {
    const ExtensionInfo* info = workbench_.extensions.find(card_);
    if (info == nullptr) {
        show_list();
        return;
    }
    // A new content widget: the old one may hold the button whose click led here.
    auto* scroll = qobject_cast<QScrollArea*>(card_page_->findChild<QScrollArea*>());
    card_content_ = new QWidget;
    card_layout_ = new QVBoxLayout(card_content_);
    card_layout_->setContentsMargins(4, 4, 4, 4);
    const l10n::Localization& localization = workbench_.localization;

    QLabel* title = make_heading(module_name(*info), card_content_);
    QFont big = title->font();
    big.setPointSizeF(big.pointSizeF() * 1.3);
    title->setFont(big);
    card_layout_->addWidget(title);
    QStringList meta{tr_text("card.version", {{"version", info->manifest->version.to_string()}})};
    if (!info->manifest->author.empty()) {
        meta.push_back(tr_text("card.author", {{"author", info->manifest->author}}));
    }
    meta.push_back(qid(info->id()));
    card_layout_->addWidget(make_muted(meta.join(QStringLiteral(" · ")), card_content_));
    const std::string_view source_key = info->source == ExtensionSource::builtin ? "card.builtin"
                                        : info->source == ExtensionSource::user  ? "card.user"
                                                                                 : "card.local";
    card_layout_->addWidget(make_label(tr_text(source_key), card_content_));

    QString state = info->enabled() ? tr_text("card.enabled") : tr_text("card.disabled");
    if (!info->compatible()) {
        state = tr_text("extensions.incompatible", {{"reason", info->incompatibility}});
    } else if (!info->activation_error.empty()) {
        state = tr_text("extensions.failed", {{"reason", info->activation_error}});
    } else if (!info->enabled() && info->code_loaded) {
        state += QLatin1Char(' ') + tr_text("card.restart");
    }
    card_layout_->addWidget(make_label(state, card_content_));
    const std::string description =
        localization.manifest_text(info->id(), info->manifest->description);
    if (!description.empty()) {
        card_layout_->addWidget(make_label(to_qstring(description), card_content_));
    }

    auto* buttons = new QHBoxLayout;
    auto* toggle = new QPushButton(
        info->enabled() ? tr_text("extensions.disable") : tr_text("extensions.enable"),
        card_content_);
    toggle->setObjectName(QStringLiteral("card.toggle"));
    toggle->setEnabled(info->compatible());
    connect(toggle, &QPushButton::clicked, this,
            [this, id = info->id(), on = !info->enabled()] { set_enabled(id, on); });
    buttons->addWidget(toggle);
    if (info->enabled() && !info->manifest->contributes.settings.empty()) {
        auto* configure = new QPushButton(tr_text("card.configure"), card_content_);
        configure->setObjectName(QStringLiteral("card.configure"));
        connect(configure, &QPushButton::clicked, this, [this, id = info->id()] {
            if (hooks_.show_settings) {
                hooks_.show_settings(id);
            }
        });
        buttons->addWidget(configure);
    }
    if (info->source != ExtensionSource::builtin) {
        auto* uninstall = new QPushButton(tr_text("card.uninstall"), card_content_);
        uninstall->setObjectName(QStringLiteral("card.uninstall"));
        connect(uninstall, &QPushButton::clicked, this,
                [this, id = info->id()] { (void)ask_uninstall(id); });
        buttons->addWidget(uninstall);
    }
    buttons->addStretch(1);
    card_layout_->addLayout(buttons);

    card_layout_->addWidget(make_heading(tr_text("card.contributes"), card_content_));
    const std::vector<ContributionLine> lines = contribution_lines(*info, localization);
    if (lines.empty()) {
        card_layout_->addWidget(make_muted(tr_text("card.contributesNothing"), card_content_));
    }
    for (const ContributionLine& line : lines) {
        card_layout_->addWidget(
            make_label(line.point + QStringLiteral(": ") + line.items.join(QStringLiteral(", ")),
                       card_content_));
    }

    const auto& requirements = info->manifest->requirements;
    if (!requirements.empty()) {
        card_layout_->addWidget(make_heading(tr_text("card.requires"), card_content_));
        for (const extension::Requirement& requirement : requirements) {
            const ExtensionInfo* needed = workbench_.extensions.find(requirement.id);
            std::string name = requirement.id;
            std::string_view key = "card.requirementMissing";
            if (needed != nullptr) {
                name = localization.manifest_text(needed->id(), needed->manifest->name);
                key = !requirement.range.contains(needed->manifest->version)
                          ? "card.requirementVersion"
                      : needed->enabled() ? "card.requirementEnabled"
                                          : "card.requirementDisabled";
            }
            card_layout_->addWidget(make_label(
                tr_text(key, {{"module", name},
                              {"range", requirement.range.to_string()},
                              {"version", needed != nullptr ? needed->manifest->version.to_string()
                                                            : std::string()}}),
                card_content_));
        }
    }
    card_layout_->addWidget(make_heading(tr_text("card.folder"), card_content_));
    // A path has no spaces: it may wrap after its separators.
    QString folder = path_text(info->folder);
    folder.replace(QDir::separator(), QString(QDir::separator()) + QChar(0x200B));
    card_layout_->addWidget(make_muted(folder, card_content_));
    card_layout_->addStretch(1);
    // The texts take the width of the panel and wrap, instead of making the card wider.
    for (QLabel* label : card_content_->findChildren<QLabel*>()) {
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    // setWidget would delete the old content at once: it is taken out and deleted later.
    QWidget* taken = scroll->takeWidget();
    scroll->setWidget(card_content_);
    if (taken != nullptr) {
        taken->deleteLater();
    }
    pages_->setCurrentWidget(card_page_);
}

void ExtensionsPanel::show_tab(Tab tab) {
    show_list();
    tabs_->setCurrentIndex(static_cast<int>(tab));
}

std::vector<std::string> ExtensionsPanel::listed() const {
    return listed_;
}

QCheckBox* ExtensionsPanel::toggle(std::string_view id) const {
    return installed_->findChild<QCheckBox*>(QStringLiteral("toggle:") + qid(id));
}

QString ExtensionsPanel::row_text(std::string_view id) const {
    const QWidget* row = installed_->findChild<QWidget*>(QStringLiteral("row:") + qid(id));
    if (row == nullptr) {
        return {};
    }
    QStringList texts;
    for (const QLabel* label : row->findChildren<QLabel*>()) {
        texts.push_back(label->text());
    }
    return texts.join(QLatin1Char('\n'));
}

std::vector<std::string> ExtensionsPanel::offered_updates() const {
    return updates_offered_;
}

QString ExtensionsPanel::status() const {
    return status_->isHidden() ? QString() : status_->text();
}

void ExtensionsPanel::show_card(const std::string& id) {
    if (workbench_.extensions.find(id) == nullptr) {
        return;
    }
    card_ = id;
    rebuild_card();
}

void ExtensionsPanel::show_list() {
    card_.clear();
    pages_->setCurrentIndex(0);
}

QString ExtensionsPanel::card_text() const {
    if (card_.empty()) {
        return {};
    }
    QStringList texts;
    for (const QLabel* label : card_content_->findChildren<QLabel*>()) {
        texts.push_back(label->text());
    }
    return texts.join(QLatin1Char('\n'));
}

// --- Enabling and disabling ----------------------------------------------------------------------

void ExtensionsPanel::set_enabled(const std::string& id, bool enabled) {
    const ExtensionInfo* info = workbench_.extensions.find(id);
    if (info == nullptr || info->enabled() == enabled) {
        schedule_rebuild();
        return;
    }
    if (enabled) {
        try {
            (void)workbench_.extensions.enable(id, extension::Journaling::on);
            set_status({});
        } catch (const extension::ExtensionError& error) {
            set_status(
                tr_text("extensions.cannotEnable",
                        {{"module", module_name(*info).toStdString()}, {"reason", error.what()}}));
        }
        changed_modules();
        return;
    }
    std::vector<std::string> plan;
    try {
        plan = workbench_.extensions.plan_disable(id);
    } catch (const extension::ExtensionError& error) {
        set_status(to_qstring(error.what()));
        schedule_rebuild();
        return;
    }
    const QString warning = objects_warning(plan);
    if (warning.isEmpty()) {
        disable_now(id);
        return;
    }
    (void)confirm(QStringLiteral("disableModuleDialog"), warning, tr_text("extensions.disable"),
                  [this, id] { disable_now(id); });
    // Until it is answered the switch shows the module as it is.
    schedule_rebuild();
}

void ExtensionsPanel::disable_now(const std::string& id) {
    try {
        (void)workbench_.extensions.disable(id, extension::Journaling::on);
        set_status({});
    } catch (const extension::ExtensionError& error) {
        set_status(to_qstring(error.what()));
    }
    changed_modules();
}

QString ExtensionsPanel::objects_warning(const std::vector<std::string>& modules) const {
    const model::Document& document = workbench_.bus.document();
    QStringList parts;
    for (const std::string& id : modules) {
        const std::size_t objects = document.find_by_owner(id).size();
        const ExtensionInfo* info = workbench_.extensions.find(id);
        if (objects == 0 || info == nullptr) {
            continue;
        }
        parts.push_back(
            tr_text("extensions.hiddenObjects", {{"count", static_cast<std::int64_t>(objects)},
                                                 {"module", module_name(*info).toStdString()}}));
    }
    QStringList also;
    for (std::size_t i = 0; i + 1 < modules.size(); ++i) {
        if (const ExtensionInfo* info = workbench_.extensions.find(modules[i])) {
            also.push_back(module_name(*info));
        }
    }
    if (parts.isEmpty() && also.isEmpty()) {
        return {};
    }
    QString warning = parts.join(QLatin1Char(' '));
    if (!parts.isEmpty()) {
        warning += QLatin1Char(' ') + tr_text("extensions.hiddenKept");
    }
    if (!also.isEmpty()) {
        if (!warning.isEmpty()) {
            warning += QStringLiteral("\n\n");
        }
        warning += tr_text("extensions.alsoDisabled",
                           {{"modules", also.join(QStringLiteral(", ")).toStdString()}});
    }
    return warning;
}

// --- Installing, uninstalling, updating ----------------------------------------------------------

void ExtensionsPanel::install_from_folder() {
    if (!workbench_.dialogs.open_folder) {
        return;
    }
    const std::filesystem::path folder =
        workbench_.dialogs.open_folder(this, tr_text("extensions.installTitle"));
    if (!folder.empty()) {
        (void)ask_install(folder);
    }
}

QMessageBox* ExtensionsPanel::ask_install(const std::filesystem::path& folder) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(folder / extension::manifest_file_name, error)) {
        set_status(tr_text("extensions.noManifest",
                           {{"folder", path_text(folder).toStdString()},
                            {"file", std::string(extension::manifest_file_name)}}));
        return nullptr;
    }
    return confirm(
        QStringLiteral("installModuleDialog"),
        tr_text("extensions.installWarning", {{"folder", path_text(folder).toStdString()}}),
        tr_text("extensions.install"), [this, folder] { install_now(folder); });
}

void ExtensionsPanel::install_now(const std::filesystem::path& folder) {
    std::string id;
    try {
        id = workbench_.extensions.install(folder, ExtensionSource::local).id();
    } catch (const std::exception& e) {
        set_status(tr_text("extensions.cannotInstall",
                           {{"folder", path_text(folder).toStdString()}, {"reason", e.what()}}));
        return;
    }
    set_status({});
    const ExtensionInfo& info = workbench_.extensions.get(id);
    if (info.compatible() && !info.enabled()) {
        try {
            (void)workbench_.extensions.enable(id, extension::Journaling::on);
        } catch (const extension::ExtensionError& error) {
            set_status(
                tr_text("extensions.cannotEnable",
                        {{"module", module_name(info).toStdString()}, {"reason", error.what()}}));
        }
    }
    changed_modules();
    card_ = id;
}

QMessageBox* ExtensionsPanel::ask_uninstall(const std::string& id) {
    const ExtensionInfo* info = workbench_.extensions.find(id);
    if (info == nullptr || info->source == ExtensionSource::builtin) {
        return nullptr;
    }
    QString text = tr_text("extensions.uninstallQuestion",
                           {{"module", module_name(*info).toStdString()},
                            {"folder", path_text(info->folder).toStdString()}});
    std::vector<std::string> plan;
    try {
        plan = workbench_.extensions.plan_disable(id);
    } catch (const extension::ExtensionError&) {
    }
    if (const QString warning = objects_warning(plan); !warning.isEmpty()) {
        text += QStringLiteral("\n\n") + warning;
    }
    return confirm(QStringLiteral("uninstallModuleDialog"), text, tr_text("card.uninstall"),
                   [this, id] {
                       try {
                           workbench_.extensions.uninstall(id);
                           set_status({});
                       } catch (const extension::ExtensionError& error) {
                           set_status(to_qstring(error.what()));
                       }
                       changed_modules();
                   });
}

void ExtensionsPanel::update_module(const std::string& id) {
    const ExtensionInfo* info = workbench_.extensions.find(id);
    if (info == nullptr) {
        return;
    }
    const QString name = module_name(*info);
    try {
        const extension::ReloadReport report = workbench_.extensions.reload(id);
        std::vector<std::string> failed;
        for (const extension::ProfileIssue& issue : report.failed) {
            failed.push_back(issue.id);
        }
        set_status(failed.empty()
                       ? tr_text("extensions.updated", {{"module", name.toStdString()}})
                       : tr_text("extensions.updatedDisabled",
                                 {{"module", name.toStdString()}, {"modules", join(failed)}}));
    } catch (const std::exception& error) {
        set_status(tr_text("extensions.cannotUpdate",
                           {{"module", name.toStdString()}, {"reason", error.what()}}));
    }
    changed_modules();
}

void ExtensionsPanel::changed_modules() {
    if (workbench_.save_modules) {
        workbench_.save_modules();
    }
    schedule_rebuild();
}

// --- Profiles ------------------------------------------------------------------------------------

std::string ExtensionsPanel::selected_profile() const {
    return profiles_box_->currentData().toString().toStdString();
}

void ExtensionsPanel::apply_profile(const std::string& name) {
    if (workbench_.profiles == nullptr) {
        return;
    }
    const std::optional<extension::Profile> profile = workbench_.profiles->find(name);
    if (!profile) {
        set_status(tr_text("profiles.notFound", {{"name", name}}));
        rebuild_profiles();
        return;
    }
    const extension::ProfilePlan plan = workbench_.extensions.plan_profile(*profile);
    QStringList hidden;
    const model::Document& document = workbench_.bus.document();
    for (const std::string& id : plan.disable) {
        const std::size_t objects = document.find_by_owner(id).size();
        if (objects > 0) {
            hidden.push_back(
                tr_text("extensions.hiddenObjects",
                        {{"count", static_cast<std::int64_t>(objects)},
                         {"module", module_name(workbench_.extensions.get(id)).toStdString()}}));
        }
    }
    if (hidden.isEmpty()) {
        apply_now(*profile);
        return;
    }
    (void)confirm(
        QStringLiteral("applyProfileDialog"),
        hidden.join(QLatin1Char(' ')) + QLatin1Char(' ') + tr_text("extensions.hiddenKept"),
        tr_text("profiles.applyButton"), [this, copy = *profile] { apply_now(copy); });
}

void ExtensionsPanel::apply_now(const extension::Profile& profile) {
    extension::ProfileReport report;
    try {
        report = workbench_.extensions.apply_profile(profile, extension::Journaling::on);
    } catch (const extension::ExtensionError& error) {
        set_status(to_qstring(error.what()));
        return;
    }
    QStringList problems;
    if (!report.missing.empty()) {
        problems.push_back(tr_text("profiles.missing", {{"modules", join(report.missing)}}));
    }
    if (!report.failed.empty()) {
        std::vector<std::string> failed;
        for (const extension::ProfileIssue& issue : report.failed) {
            failed.push_back(issue.id);
        }
        problems.push_back(tr_text("profiles.failed", {{"modules", join(failed)}}));
    }
    if (!profile.layout.empty() && hooks_.restore_layout &&
        !hooks_.restore_layout(profile.layout)) {
        problems.push_back(tr_text("profiles.badLayout"));
    }
    const QString message = problems.isEmpty()
                                ? tr_text("profiles.applied", {{"name", profile.name}})
                                : problems.join(QLatin1Char(' '));
    set_status(problems.isEmpty() ? QString() : message);
    if (hooks_.report) {
        hooks_.report(message);
    }
    changed_modules();
}

void ExtensionsPanel::save_profile(const std::string& name) {
    if (workbench_.profiles == nullptr || name.empty()) {
        return;
    }
    nlohmann::json layout = hooks_.layout ? hooks_.layout() : nlohmann::json::object();
    try {
        (void)workbench_.profiles->save(workbench_.extensions.profile(name, std::move(layout)));
        set_status({});
    } catch (const std::exception& error) {
        set_status(tr_text("profiles.cannotSave", {{"name", name}, {"reason", error.what()}}));
    }
    rebuild_profiles();
    profiles_box_->setCurrentIndex(std::max(0, profiles_box_->findData(to_qstring(name))));
}

QWidget* ExtensionsPanel::ask_profile_name() {
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("profileNameDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr_text("profiles.saveAs").remove(QStringLiteral("…")));
    dialog->setLabelText(tr_text("profiles.name"));
    dialog->setTextValue(to_qstring(selected_profile()));
    dialog->setOkButtonText(tr_text("dialog.ok"));
    dialog->setCancelButtonText(tr_text("dialog.cancel"));
    connect(dialog, &QInputDialog::textValueSelected, this, [this](const QString& value) {
        const QString name = value.trimmed();
        if (!name.isEmpty()) {
            save_profile(name.toStdString());
        }
    });
    dialog->open();
    return dialog;
}

void ExtensionsPanel::delete_profile(const std::string& name) {
    if (workbench_.profiles == nullptr) {
        return;
    }
    try {
        (void)workbench_.profiles->remove(name);
        set_status({});
    } catch (const std::exception& error) {
        set_status(to_qstring(error.what()));
    }
    rebuild_profiles();
}

void ExtensionsPanel::import_profile() {
    if (workbench_.profiles == nullptr || !workbench_.dialogs.open_file) {
        return;
    }
    const std::filesystem::path file = workbench_.dialogs.open_file(
        this, tr_text("profiles.importTitle"), tr_text("profiles.filter"));
    if (file.empty()) {
        return;
    }
    try {
        const extension::Profile profile = extension::load_profile(file);
        (void)workbench_.profiles->save(profile);
        set_status({});
        rebuild_profiles();
        profiles_box_->setCurrentIndex(
            std::max(0, profiles_box_->findData(to_qstring(profile.name))));
    } catch (const std::exception& error) {
        set_status(tr_text("profiles.cannotImport",
                           {{"file", path_text(file).toStdString()}, {"reason", error.what()}}));
    }
}

void ExtensionsPanel::export_profile(const std::string& name) {
    if (workbench_.profiles == nullptr || !workbench_.dialogs.save_file) {
        return;
    }
    const std::optional<extension::Profile> profile = workbench_.profiles->find(name);
    if (!profile) {
        return;
    }
    std::filesystem::path file = workbench_.dialogs.save_file(
        this, tr_text("profiles.exportTitle"),
        to_qstring(extension::ProfileLibrary::file_name(name)), tr_text("profiles.filter"));
    if (file.empty()) {
        return;
    }
    if (!file.has_extension()) {
        file.replace_extension(".json");
    }
    try {
        extension::save_profile(file, *profile);
        set_status({});
    } catch (const std::exception& error) {
        set_status(tr_text("profiles.cannotExport",
                           {{"file", path_text(file).toStdString()}, {"reason", error.what()}}));
    }
}

// --- Helpers -------------------------------------------------------------------------------------

QMessageBox* ExtensionsPanel::confirm(const QString& name, const QString& text,
                                      const QString& accept, std::function<void()> accepted) {
    auto* box = new QMessageBox(QMessageBox::Warning, tr_text("extensions.title"), text,
                                QMessageBox::NoButton, this);
    box->setObjectName(name);
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* ok = box->addButton(accept, QMessageBox::AcceptRole);
    ok->setObjectName(QStringLiteral("accept"));
    QPushButton* cancel = box->addButton(tr_text("dialog.cancel"), QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cancel"));
    box->setDefaultButton(cancel);
    connect(box, &QMessageBox::buttonClicked, this,
            [ok, accepted = std::move(accepted)](QAbstractButton* button) {
                if (button == ok) {
                    accepted();
                }
            });
    box->open();
    return box;
}

void ExtensionsPanel::set_status(const QString& message) {
    status_->setText(message);
    status_->setVisible(!message.isEmpty());
    if (!message.isEmpty() && hooks_.report) {
        hooks_.report(message);
    }
}

QString ExtensionsPanel::module_name(const ExtensionInfo& info) const {
    return to_qstring(workbench_.localization.manifest_text(info.id(), info.manifest->name));
}

void ExtensionsPanel::retranslate() {
    search_->setPlaceholderText(tr_text("extensions.search"));
    install_->setText(tr_text("extensions.installFolder"));
    tabs_->setTabText(static_cast<int>(Tab::installed), tr_text("extensions.tabInstalled"));
    tabs_->setTabText(static_cast<int>(Tab::catalog), tr_text("extensions.tabCatalog"));
    catalog_->setText(tr_text("extensions.catalogEmpty"));
    no_updates_->setText(tr_text("extensions.noUpdates"));
    profiles_label_->setText(tr_text("profiles.title"));
    apply_->setText(tr_text("profiles.applyButton"));
    more_->setToolTip(tr_text("profiles.more"));
    for (QAction* action : more_->menu()->actions()) {
        if (!action->isSeparator()) {
            action->setText(tr_text(action->objectName().toStdString()));
        }
    }
    back_->setText(tr_text("card.back"));
    rebuild();
}

QString ExtensionsPanel::tr_text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

}  // namespace fem::shell
