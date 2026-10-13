#pragma once

#include <QListWidget>
#include <QString>

namespace fem::shell {

/// The content of the "Problems" tab of the bottom panel: messages for the user, the newest
/// first, such as a module whose code crashed or could not start. Problems of the model come with
/// its checks (later tasks).
class ProblemsPanel : public QListWidget {
    Q_OBJECT

public:
    explicit ProblemsPanel(QWidget* parent = nullptr);

    /// Adds a message as the newest problem.
    void add(const QString& message);
    /// The messages, the newest first.
    [[nodiscard]] QStringList messages() const;

Q_SIGNALS:
    /// A problem was added.
    void changed();
};

}  // namespace fem::shell
