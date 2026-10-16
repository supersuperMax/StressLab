#include "fem/core/io/files.h"
#include "fem/storage/blob_store.h"
#include "fem/storage/error.h"
#include "fem/storage/sha256.h"

#include "storage_test_support.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

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
    EXPECT_EQ(store.bytes(), 4U);
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
    // A zstd frame of the contents; the hash is that of the contents, not of the file.
    const std::string frame = io::read_file(file).value();
    ASSERT_GE(frame.size(), 4U);
    EXPECT_EQ(frame.substr(0, 4), "\x28\xb5\x2f\xfd");

    // The same contents again do not touch the file.
    const auto written = fs::last_write_time(file);
    EXPECT_EQ(store.put(bytes_of("abc")), ref);
    EXPECT_EQ(fs::last_write_time(file), written);

    const Bytes back = store.get(ref);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()), "abc");
    EXPECT_TRUE(store.contains(ref));
    EXPECT_FALSE(store.contains(blob_ref(bytes_of("abd"))));
    EXPECT_FALSE(store.contains("not a ref"));
}

TEST(StorageBlobs, FolderStoreCompresses) {
    TempDir dir("blobs-compressed");
    FolderBlobStore store(dir.path());
    std::string text;
    for (int i = 0; i < 10000; ++i) {
        text += "node " + std::to_string(i % 100) + "\n";
    }
    const std::string ref = store.put(bytes_of(text));
    EXPECT_LT(fs::file_size(store.file_of(ref)), text.size() / 10);
    const Bytes back = store.get(ref);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()), text);

    const std::string empty = store.put({});
    EXPECT_TRUE(store.get(empty).empty());
}

TEST(StorageBlobs, FolderStoreFindsDamagedBlocks) {
    TempDir dir("blobs-damaged");
    FolderBlobStore store(dir.path());
    const std::string ref = store.put(bytes_of("abc"));
    const std::string frame = io::read_file(store.file_of(ref)).value();
    const auto expect_corrupt = [&](const std::string& bytes, const char* what) {
        ASSERT_TRUE(io::write_file(store.file_of(ref), bytes));
        try {
            (void)store.get(ref);
            ADD_FAILURE() << what;
        } catch (const ProjectError& error) {
            EXPECT_EQ(error.code(), ProjectErrc::corrupt_blob) << what;
        }
    };
    expect_corrupt("abd", "not zstd");
    expect_corrupt(frame.substr(0, frame.size() - 1), "a truncated frame");
    expect_corrupt(frame + frame, "two frames");
    expect_corrupt(frame + "x", "bytes after the frame");
    // A whole frame of other contents: zstd is happy, the hash is not.
    FolderBlobStore other(dir.path() / "other");
    expect_corrupt(io::read_file(other.file_of(other.put(bytes_of("abd")))).value(),
                   "other contents");
    fs::remove(store.file_of(ref));
    try {
        (void)store.get(ref);
        FAIL();
    } catch (const ProjectError& error) {
        EXPECT_EQ(error.code(), ProjectErrc::missing_blob);
    }
}

TEST(StorageBlobs, FolderStoreListsItsFiles) {
    TempDir dir("blobs-files");
    FolderBlobStore store(dir.path() / "objects");
    EXPECT_TRUE(store.files().empty());  // No folder yet.
    const std::string b = store.put(bytes_of("b"));
    const std::string a = store.put(bytes_of("a"));
    // Leftovers: a temporary file of a crash and a file that is not where a block would be.
    fs::path tmp = store.file_of(a);
    tmp += ".tmp";
    ASSERT_TRUE(io::write_file(tmp, "half"));
    const fs::path stray = dir.path() / "objects" / "sha256" / "stray";
    ASSERT_TRUE(io::write_file(stray, "?"));

    const std::vector<FolderBlobStore::File> files = store.files();
    ASSERT_EQ(files.size(), 4U);
    const std::string first = std::min(a, b);
    const std::string second = std::max(a, b);
    EXPECT_EQ(files[0].ref, first);
    EXPECT_EQ(files[1].ref, second);
    EXPECT_EQ(files[0].path, store.file_of(first));
    EXPECT_EQ(files[0].size, fs::file_size(store.file_of(first)));
    EXPECT_TRUE(files[2].ref.empty());
    EXPECT_TRUE(files[3].ref.empty());

    EXPECT_TRUE(store.remove(store.file_of(a)));
    EXPECT_FALSE(store.contains(a));
    EXPECT_FALSE(store.remove(store.file_of(a)));
    EXPECT_TRUE(store.contains(b));
}

}  // namespace
}  // namespace fem::storage
