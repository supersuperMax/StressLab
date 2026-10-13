// Named shared memory (task 1.9): what one mapping writes another one sees, names, unlinking.

#include "fem/core/ipc/shared_memory.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <utility>

namespace {

using fem::ipc::SharedMemory;
using fem::ipc::SharedMemoryError;

TEST(SharedMemory, OpenedSegmentSharesTheMemory) {
    SharedMemory created = SharedMemory::create(1 << 20);
    EXPECT_TRUE(created.owner());
    EXPECT_GE(created.size(), std::size_t{1} << 20);
    EXPECT_LE(created.name().size(), 30U);  // macOS allows 31 characters with the '/'.
    EXPECT_EQ(created.data()[12345], std::byte{0});

    const char text[] = "общая память";
    std::memcpy(created.data() + 1000, text, sizeof(text));
    const SharedMemory opened = SharedMemory::open(created.name(), 2000);
    EXPECT_FALSE(opened.owner());
    EXPECT_STREQ(reinterpret_cast<const char*>(opened.data() + 1000), text);
    opened.data()[5] = std::byte{42};
    EXPECT_EQ(created.data()[5], std::byte{42});
}

TEST(SharedMemory, NamesAreUnique) {
    std::set<std::string> names;
    std::vector<SharedMemory> segments;
    for (int i = 0; i < 20; ++i) {
        segments.push_back(SharedMemory::create(16));
        names.insert(segments.back().name());
    }
    EXPECT_EQ(names.size(), segments.size());
}

TEST(SharedMemory, UnlinkedSegmentStaysMappedButCannotBeOpened) {
    SharedMemory created = SharedMemory::create(64);
    SharedMemory opened = SharedMemory::open(created.name(), 64);
    const std::string name = created.name();
    created.unlink();
    created.unlink();
    EXPECT_THROW((void)SharedMemory::open(name, 64), SharedMemoryError);
    opened.data()[0] = std::byte{7};
    EXPECT_EQ(created.data()[0], std::byte{7});
}

TEST(SharedMemory, CreatorRemovesTheNameWhenDestroyed) {
    std::string name;
    {
        const SharedMemory created = SharedMemory::create(64);
        name = created.name();
    }
    EXPECT_THROW((void)SharedMemory::open(name, 64), SharedMemoryError);
}

TEST(SharedMemory, RefusesMissingSmallerAndBadlyNamedSegments) {
    EXPECT_THROW((void)SharedMemory::open("sl_no_such_segment", 8), SharedMemoryError);
    const SharedMemory created = SharedMemory::create(100);
    EXPECT_THROW((void)SharedMemory::open(created.name(), 100 << 20), SharedMemoryError);
    for (const char* bad :
         {"", "a/b", "a\\b", "with space", "a_name_that_is_much_too_long_for_macos"}) {
        EXPECT_THROW((void)SharedMemory::open(bad, 8), SharedMemoryError) << bad;
    }
}

TEST(SharedMemory, MovesWithoutUnlinking) {
    SharedMemory created = SharedMemory::create(32);
    const std::string name = created.name();
    SharedMemory moved = std::move(created);
    SharedMemory assigned = SharedMemory::create(8);
    assigned = std::move(moved);
    EXPECT_EQ(assigned.name(), name);
    EXPECT_NO_THROW((void)SharedMemory::open(name, 32));
}

}  // namespace
