#include "fem/shell/workspaces.h"

#include "fem/core/log/log.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <stdexcept>
#include <string>
#include <utility>

namespace fem::shell {

namespace {

const QString format_name = QStringLiteral("stresslab.workspaces");
constexpr int format_version = 1;

std::string display(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.generic_u16string());
}

QJsonObject layout_json(const Layout& layout) {
    return QJsonObject{
        {QStringLiteral("bars"), QString::fromLatin1(layout.bars.toBase64())},
        {QStringLiteral("docks"), QString::fromUtf8(layout.docks)},
    };
}

Layout layout_from(const QJsonValue& value, const QString& where) {
    if (!value.isObject()) {
        throw std::runtime_error(where.toStdString() + " is not an object");
    }
    const QJsonObject object = value.toObject();
    const QJsonValue bars = object.value(QStringLiteral("bars"));
    const QJsonValue docks = object.value(QStringLiteral("docks"));
    if (!bars.isString() || !docks.isString()) {
        throw std::runtime_error(where.toStdString() + " needs the strings 'bars' and 'docks'");
    }
    return {.docks = docks.toString().toUtf8(),
            .bars = QByteArray::fromBase64(bars.toString().toLatin1())};
}

}  // namespace

WorkspaceStore::WorkspaceStore(std::filesystem::path file) : file_(std::move(file)) {
    QFile input(to_qstring(file_));
    if (!input.exists()) {
        return;
    }
    try {
        if (!input.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(input.errorString().toStdString());
        }
        WorkspaceStore loaded = from_json(input.readAll());
        loaded.file_ = std::move(file_);
        *this = std::move(loaded);
    } catch (const std::exception& error) {
        log::logger("shell")->warn("the workspaces in '{}' are not used: {}", display(file_),
                                   error.what());
    }
}

QByteArray WorkspaceStore::to_json() const {
    QJsonObject workspaces;
    for (auto it = workspaces_.cbegin(); it != workspaces_.cend(); ++it) {
        workspaces.insert(it.key(), layout_json(it.value()));
    }
    // QJsonObject keeps its keys sorted, and numbers here are integers: the bytes depend on the
    // content only.
    QJsonObject root{
        {QStringLiteral("current"), current_},
        {QStringLiteral("format"), format_name},
        {QStringLiteral("formatVersion"), format_version},
        {QStringLiteral("workspaces"), workspaces},
    };
    if (session_) {
        QJsonObject session = layout_json(*session_);
        session.insert(QStringLiteral("geometry"), QString::fromLatin1(geometry_.toBase64()));
        root.insert(QStringLiteral("session"), session);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

WorkspaceStore WorkspaceStore::from_json(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        throw std::runtime_error("not JSON: " + error.errorString().toStdString());
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != format_name ||
        root.value(QStringLiteral("formatVersion")).toInt(-1) != format_version) {
        throw std::runtime_error("not a file of workspaces of format version 1");
    }
    WorkspaceStore store;
    store.current_ = root.value(QStringLiteral("current")).toString();
    const QJsonObject workspaces = root.value(QStringLiteral("workspaces")).toObject();
    for (auto it = workspaces.constBegin(); it != workspaces.constEnd(); ++it) {
        store.workspaces_.insert(
            it.key(), layout_from(it.value(), QStringLiteral("workspace '%1'").arg(it.key())));
    }
    if (root.contains(QStringLiteral("session"))) {
        const QJsonValue session = root.value(QStringLiteral("session"));
        store.session_ = layout_from(session, QStringLiteral("the session"));
        store.geometry_ = QByteArray::fromBase64(
            session.toObject().value(QStringLiteral("geometry")).toString().toLatin1());
    }
    if (!store.current_.isEmpty() && !store.workspaces_.contains(store.current_)) {
        store.current_.clear();
    }
    return store;
}

bool WorkspaceStore::save() const {
    if (file_.empty()) {
        return true;
    }
    std::error_code ignored;
    std::filesystem::create_directories(file_.parent_path(), ignored);
    QSaveFile output(to_qstring(file_));
    if (!output.open(QIODevice::WriteOnly) || output.write(to_json()) < 0 || !output.commit()) {
        log::logger("shell")->error("the workspaces cannot be saved to '{}': {}", display(file_),
                                    output.errorString().toStdString());
        return false;
    }
    return true;
}

std::optional<Layout> WorkspaceStore::find(const QString& name) const {
    const auto it = workspaces_.constFind(name);
    if (it == workspaces_.cend()) {
        return std::nullopt;
    }
    return it.value();
}

void WorkspaceStore::set(const QString& name, const Layout& layout) {
    workspaces_.insert(name, layout);
}

bool WorkspaceStore::remove(const QString& name) {
    if (workspaces_.remove(name) == 0) {
        return false;
    }
    if (current_ == name) {
        current_.clear();
    }
    return true;
}

void WorkspaceStore::set_session(const Layout& layout, const QByteArray& geometry) {
    session_ = layout;
    geometry_ = geometry;
}

}  // namespace fem::shell
