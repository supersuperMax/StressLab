#pragma once

#include "fem/python/console.h"
#include "fem/shell/python_console.h"

namespace fem::app {

/// The Python console of the window on the embedded interpreter (task 1.14): what the shell types
/// goes to `python::Console`. Errors of the console itself come back as error output.
class PythonConsoleEngine final : public shell::ConsoleEngine {
public:
    /// Needs a running `python::Interpreter`, which must outlive it. Throws PythonError.
    PythonConsoleEngine() = default;

    [[nodiscard]] QString banner() const override;
    Reply push(const QString& line) override;
    void reset() override;
    [[nodiscard]] QStringList complete(const QString& line) override;

private:
    python::Console console_;
};

}  // namespace fem::app
