#pragma once

#include "fem/core/model/object_id.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fem::model {

/// Net effect of a group of changes, for listeners of "the model changed". Each list is sorted
/// and an object is in at most one of them.
struct ChangeSummary {
    std::vector<ObjectId> created;  ///< Did not exist before the changes and exist after them.
    std::vector<ObjectId> removed;  ///< Existed before the changes and do not exist after them.
    std::vector<ObjectId> updated;  ///< Exist before and after, and something in them changed.
    bool units = false;             ///< The unit system changed.

    [[nodiscard]] bool empty() const noexcept {
        return created.empty() && removed.empty() && updated.empty() && !units;
    }

    friend bool operator==(const ChangeSummary&, const ChangeSummary&) = default;
};

/// Reversible record of the changes a `DocumentEditor` made to a document.
///
/// Every record holds the value it replaced, so the changes can be applied backwards (undo) and
/// forwards again (redo) any number of times, as long as the document is in the state they left
/// it in. Removed objects are kept whole, so undo restores them with the same ids, data and place
/// in the tree. Change sets are created and applied by the Command Bus.
class ChangeSet {
public:
    ChangeSet(ChangeSet&& other) noexcept;
    ChangeSet& operator=(ChangeSet&& other) noexcept;
    ChangeSet(const ChangeSet&) = delete;
    ChangeSet& operator=(const ChangeSet&) = delete;
    ~ChangeSet();

    // Defined out of line: `Change` is complete only in the implementation.
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /// What the changes did the last time they were applied: executed, undone or redone.
    [[nodiscard]] ChangeSummary summary() const;

    /// Moves the records of `later` to the end of this set. `later` must have been recorded right
    /// after this set, and neither may be undone.
    void append(ChangeSet&& later);

private:
    friend class Document;
    struct Change;

    explicit ChangeSet(std::uint64_t next_id) noexcept;

    std::vector<Change> changes_;
    std::uint64_t next_id_;  // The document's next object id before the first record.
    bool undone_ = false;    // Last applied backwards.
};

}  // namespace fem::model
