#pragma once

namespace fem::task {

/// A long operation that runs in the background (docs/architecture.md §4.6, item 7). A command
/// may return one instead of finishing before it returns; the shell then shows it in "Jobs".
///
/// Stub of task 1.3: the Command Bus already passes tasks through to the caller. Progress,
/// messages, cancellation, the result and errors are defined in task 1.6.
class Task {
public:
    virtual ~Task() = default;

protected:
    Task() = default;
    Task(const Task&) = default;
    Task(Task&&) = default;
    Task& operator=(const Task&) = default;
    Task& operator=(Task&&) = default;
};

}  // namespace fem::task
