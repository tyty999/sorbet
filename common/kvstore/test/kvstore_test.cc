#include "gtest/gtest.h"
// has to go first as it violates our requirements
#include "spdlog/spdlog.h"
// has to go above null_sink.h; this comment prevents reordering.
#include "absl/strings/str_split.h" // For StripAsciiWhitespace
#include "common/FileOps.h"
#include "common/common.h"
#include "common/kvstore/KeyValueStore.h"
#include "spdlog/sinks/null_sink.h"

using namespace std;
using namespace sorbet;

string exec(string cmd);

namespace {
class KeyValueStoreTest : public ::testing::Test {
protected:
    const string directory;
    const shared_ptr<spdlog::sinks::null_sink_mt> sink;
    const shared_ptr<spdlog::logger> logger;

    KeyValueStoreTest()
        : directory(string(absl::StripAsciiWhitespace(exec("mktemp -d")))),
          sink(make_shared<spdlog::sinks::null_sink_mt>()), logger(make_shared<spdlog::logger>("null", sink)) {}

    void TearDown() override {
        exec(fmt::format("rm -r {}", directory));
    }

    // Inspired by https://github.com/google/googletest/issues/1153#issuecomment-428247477
    int wait_for_child_fork(int pid) {
        int status;
        if (0 > waitpid(pid, &status, 0)) {
            std::cerr << "[----------]  Waitpid error!" << std::endl;
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            std::cerr << "[----------]  Non-normal exit from child!" << std::endl;
            return -2;
        }
    }
};

} // namespace

TEST_F(KeyValueStoreTest, CommitsChangesToDisk) {
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        owned->writeString("hello", "testing");
        EXPECT_EQ(owned->readString("hello"), "testing");
        OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
    }
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        EXPECT_EQ(owned->readString("hello"), "testing");
    }
}

TEST_F(KeyValueStoreTest, AbortsChangesByDefault) {
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        owned->writeString("hello", "testing");
        EXPECT_EQ(owned->readString("hello"), "testing");
    }
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        EXPECT_EQ(owned->readString("hello"), "");
    }
}

TEST_F(KeyValueStoreTest, CanBeReowned) {
    auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
    auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
    owned->writeString("hello", "testing");
    EXPECT_EQ(owned->readString("hello"), "testing");
    kvstore = OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
    owned = make_unique<OwnedKeyValueStore>(move(kvstore));
    EXPECT_EQ(owned->readString("hello"), "testing");
}

TEST_F(KeyValueStoreTest, AbortsChangesWhenAborted) {
    auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
    auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
    owned->writeString("hello", "testing");
    EXPECT_EQ(owned->readString("hello"), "testing");
    kvstore = OwnedKeyValueStore::abort(move(owned));
    owned = make_unique<OwnedKeyValueStore>(move(kvstore));
    EXPECT_EQ(owned->readString("hello"), "");
}

TEST_F(KeyValueStoreTest, ClearsChangesOnVersionChange) {
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        owned->writeString("hello", "testing");
        EXPECT_EQ(owned->readString("hello"), "testing");
        OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
    }
    {
        auto kvstore = make_unique<KeyValueStore>("2", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        EXPECT_EQ(owned->readString("hello"), "");
    }
}

TEST_F(KeyValueStoreTest, FlavorsHaveDifferentContents) {
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        owned->writeString("hello", "testing");
        EXPECT_EQ(owned->readString("hello"), "testing");
        OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
    }
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "coldbrewcoffeewithchocolateflakes");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        EXPECT_EQ(owned->readString("hello"), "");
    }
}

TEST_F(KeyValueStoreTest, ReadOnlyTransactionsSeeConsistentViewOfStore) {
    {
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto owned = make_unique<OwnedKeyValueStore>(move(kvstore));
        owned->writeString("hello", "testing");
        EXPECT_EQ(owned->readString("hello"), "testing");
        OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
    }
    {
        // Begin a read-only transaction.
        auto kvstore = make_unique<KeyValueStore>("1", directory, "vanilla");
        auto readOnly = make_unique<ReadOnlyKeyValueStore>(move(kvstore));
        EXPECT_EQ(readOnly->readString("hello"), "testing");

        // Fork a thread that writes over the testing key.
        // We _have_ to fork; lmdb makes assumptions about how it is used within a single process.
        const int pid = fork();
        if (pid == 0) {
            // Child -- needs to exit the process at the end to avoid running the rest of the tests.
            auto kvstoreWrite = make_unique<KeyValueStore>("1", directory, "vanilla");
            auto owned = make_unique<OwnedKeyValueStore>(move(kvstoreWrite));
            EXPECT_EQ(owned->readString("hello"), "testing");
            owned->writeString("hello", "overwritten");
            OwnedKeyValueStore::bestEffortCommit(*logger, move(owned));
            exit(testing::Test::HasFailure());
        } else {
            // Wait for write in other process to complete before proceeding.
            ASSERT_EQ(0, wait_for_child_fork(pid));

            // The write in the other process should have no bearing on reads in this process.
            EXPECT_EQ(readOnly->readString("hello"), "testing");

            // Verify that worker threads see the same data.
            {
                // Destructor for return value waits for thread to complete.
                runInAThread("workerThread", [&readOnly]() {
                    // Stufff
                    EXPECT_EQ(readOnly->readString("hello"), "testing");
                });
            }
            EXPECT_EQ(readOnly->readString("hello"), "testing");
        }
    }
}
