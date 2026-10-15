#include "fem/core/io/files.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/sha256.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace fem::storage {
namespace {

using namespace testing;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// FIPS 180-4 examples and NIST CAVS short messages.
TEST(StorageSha256, KnownDigests) {
    EXPECT_EQ(sha256_hex(bytes_of("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex(bytes_of("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256_hex(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    const std::string million(1000000, 'a');
    EXPECT_EQ(sha256_hex(bytes_of(million)),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(StorageSha256, PiecesGiveTheSameDigestAsTheWhole) {
    std::string text;
    for (int i = 0; i < 300; ++i) {
        text.push_back(static_cast<char>(i * 7));
    }
    // Every split around the 55/56/64-byte edges of padding and blocks.
    for (std::size_t length = 0; length <= text.size(); length += 1) {
        const std::string_view whole(text.data(), length);
        for (const std::size_t split : {std::size_t{0}, length / 3, length / 2, length}) {
            Sha256 hash;
            hash.update(bytes_of(whole.substr(0, split)));
            hash.update(bytes_of(whole.substr(split)));
            const Sha256::Digest digest = hash.finish();
            Sha256 once;
            once.update(bytes_of(whole));
            ASSERT_EQ(digest, once.finish()) << "length " << length << " split " << split;
        }
    }
}

TEST(StorageBlobs, ReferencesAreSha256OfTheContents) {
    EXPECT_EQ(blob_ref(bytes_of("abc")),
              "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(is_blob_ref(blob_ref(bytes_of("x"))));
    EXPECT_FALSE(
        is_blob_ref("sha256:BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    EXPECT_FALSE(
        is_blob_ref("sha1:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    EXPECT_FALSE(is_blob_ref("sha256:ba78"));
}

TEST(StorageBlobs, MemoryStoreKeepsEachContentOnce) {
    MemoryBlobStore store;
    const std::string ref = store.put(bytes_of("mesh"));
    EXPECT_EQ(store.put(bytes_of("mesh")), ref);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_TRUE(store.contains(ref));
    const Bytes back = store.get(ref);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()), "mesh");
    try {
        (void)store.get(blob_ref(bytes_of("other")));
        FAIL();
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::missing_blob);
    }
}

TEST(StorageBlobs, FolderStoreLaysOutBlocksByHash) {
    TempDir dir("blobs");
    FolderBlobStore store(dir.path() / "objects");
    const std::string ref = store.put(bytes_of("abc"));
    const fs::path file = dir.path() / "objects" / "sha256" / "ba" /
                          "7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    EXPECT_EQ(store.file_of(ref), file);
    EXPECT_TRUE(fs::is_regular_file(file));
    EXPECT_EQ(io::read_file(file), "abc");

    // The same contents again do not touch the file.
    const auto written = fs::last_write_time(file);
    EXPECT_EQ(store.put(bytes_of("abc")), ref);
    EXPECT_EQ(fs::last_write_time(file), written);

    const Bytes back = store.get(ref);
    EXPECT_EQ(back.size(), 3U);
    EXPECT_TRUE(store.contains(ref));
    EXPECT_FALSE(store.contains(blob_ref(bytes_of("abd"))));
    EXPECT_FALSE(store.contains("not a ref"));
}

TEST(StorageBlobs, FolderStoreFindsDamagedBlocks) {
    TempDir dir("blobs-damaged");
    FolderBlobStore store(dir.path());
    const std::string ref = store.put(bytes_of("abc"));
    ASSERT_TRUE(io::write_file(store.file_of(ref), "abd"));
    try {
        (void)store.get(ref);
        FAIL();
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::corrupt_blob);
    }
    fs::remove(store.file_of(ref));
    try {
        (void)store.get(ref);
        FAIL();
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::missing_blob);
    }
}

}  // namespace
}  // namespace fem::storage
