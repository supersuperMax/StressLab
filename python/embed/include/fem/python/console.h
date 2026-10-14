#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fem::python {

/// A piece of what code run in the console printed.
struct ConsoleOutput {
    bool error = false;     ///< Printed to standard error (also tracebacks).
    std::string text = {};  ///< UTF-8.

    friend bool operator==(const ConsoleOutput&, const ConsoleOutput&) = default;
};

/// What `Console::push` did with a line.
struct ConsoleReply {
    /// The line starts or continues a statement that is not complete yet: more lines are needed.
    bool more = false;
    /// What was printed while the line ran, in order.
    std::vector<ConsoleOutput> output = {};
};

/// The interactive Python console of the window (task 1.14): `stresslab._console.Console` of the
/// embedded interpreter, with `stresslab` imported in its own namespace. Lines run on the thread
/// that calls `push`, with the GIL; the attached document and context keys are what
/// `stresslab.cmd` works with, so commands run from the console are journaled like any others.
///
/// Needs a running `Interpreter`, which must outlive it.
class Console {
public:
    /// Throws PythonError if the console cannot be made.
    Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;
    ~Console();

    /// The versions of Python and of `stresslab`.
    [[nodiscard]] std::string banner() const;
    /// Runs a line, or keeps it until the statement it starts is complete. Errors of the code are
    /// output; throws PythonError only if the console itself fails.
    ConsoleReply push(std::string_view line);
    /// Drops the lines of a statement that is not complete yet.
    void reset();
    /// The lines Tab can make of `line`, completing the name or attribute at its end.
    [[nodiscard]] std::vector<std::string> complete(std::string_view line);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace fem::python
