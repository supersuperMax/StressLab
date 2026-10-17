#pragma once

#include <functional>

namespace fem::storage {

/// How a long operation of a project (packing, unpacking, pinning a result: task 2.5) reports how
/// far it is and learns that it should stop. A shell runs the operation as a `task::Task` and
/// connects these to its `TaskControl`; a caller that needs neither leaves them empty.
struct Progress {
    /// Called with the part done, from 0 to 1, on the thread of the operation.
    std::function<void(double)> report{};
    /// Asked between steps; true stops the operation with ProjectError(cancelled). The project is
    /// left as it was, but for blocks it stored already, which go with the next "Clean project".
    std::function<bool()> cancelled{};

    void step(double done) const {
        if (report) {
            report(done);
        }
    }

    /// Throws ProjectError(cancelled) if the operation is asked to stop.
    void check() const;
};

}  // namespace fem::storage
