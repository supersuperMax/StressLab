#include "fem/shell/required_modules_bar.h"

#include "fem/core/extension/error.h"
#include "fem/core/log/log.h"
#include "fem/shell/strings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace fem::shell {

namespace {

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

RequiredModulesBar::RequiredModulesBar(Workbench& workbench, QWidget* parent)
    : QFrame(parent), workbench_(workbench) {
    setObjectName(QStringLiteral("requiredModules"));
    setFrameShape(QFrame::StyledPanel);
    setBackgroundRole(QPalette::AlternateBase);
    setAutoFillBackground(true);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("requiredModules.text"));
    text_->setTextFormat(Qt::PlainText);
    text_->setWordWrap(true);
    enable_ = new QPushButton(this);
    enable_->setObjectName(QStringLiteral("requiredModules.enable"));
    connect(enable_, &QPushButton::clicked, this, &RequiredModulesBar::enable_modules);
    dismiss_ = new QPushButton(this);
    dismiss_->setObjectName(QStringLiteral("requiredModules.dismiss"));
    dismiss_->setFlat(true);
    connect(dismiss_, &QPushButton::clicked, this, &RequiredModulesBar::dismiss);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->addWidget(text_, 1);
    layout->addWidget(enable_);
    layout->addWidget(dismiss_);
    hide();
    retranslate();
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
}

RequiredModulesBar::~RequiredModulesBar() = default;

void RequiredModulesBar::check() {
    modules_.clear();
    std::vector<std::string> missing;
    for (const extension::NeededModule& needed : workbench_.extensions.needed_modules()) {
        if (needed.can_enable()) {
            modules_.push_back(needed.id);
        } else {
            missing.push_back(needed.id);
        }
    }
    const std::size_t installed = modules_.size();
    modules_.insert(modules_.end(), missing.begin(), missing.end());
    enable_->setVisible(installed > 0);
    retranslate();
    setVisible(!modules_.empty());
}

void RequiredModulesBar::enable_modules() {
    std::vector<std::string> failed;
    for (const extension::NeededModule& needed : workbench_.extensions.needed_modules()) {
        if (!needed.can_enable()) {
            continue;
        }
        try {
            (void)workbench_.extensions.enable(needed.id, extension::Journaling::on);
        } catch (const extension::ExtensionError& error) {
            log::logger("shell")->error("module '{}' cannot be enabled: {}", needed.id,
                                        error.what());
            failed.push_back(needed.id);
        }
    }
    if (workbench_.save_modules) {
        workbench_.save_modules();
    }
    if (!failed.empty() && report_) {
        report_(
            text(workbench_.localization, "requiredModules.failed", {{"modules", join(failed)}}));
    }
    check();
}

void RequiredModulesBar::dismiss() {
    hide();
}

QString RequiredModulesBar::message() const {
    return text_->text();
}

void RequiredModulesBar::retranslate() {
    const l10n::Localization& localization = workbench_.localization;
    std::vector<std::string> names;
    std::vector<std::string> missing;
    for (const std::string& id : modules_) {
        const extension::ExtensionInfo* info = workbench_.extensions.find(id);
        if (info != nullptr && info->compatible()) {
            names.push_back(localization.manifest_text(id, info->manifest->name));
        } else {
            missing.push_back(id);
        }
    }
    QString message;
    if (!names.empty()) {
        message = text(localization, "requiredModules.needs", {{"modules", join(names)}});
    }
    if (!missing.empty()) {
        if (!message.isEmpty()) {
            message += QLatin1Char(' ');
        }
        message += text(localization, "requiredModules.missing", {{"modules", join(missing)}});
    }
    text_->setText(message);
    enable_->setText(text(localization, "requiredModules.enableButton"));
    dismiss_->setText(text(localization, "requiredModules.dismissButton"));
}

}  // namespace fem::shell
