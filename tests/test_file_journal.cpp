#include <hpc/ipc/file_journal.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using hpc::ipc::file_journal;

namespace {

struct record {
    std::uint64_t seq;
    double        value;
};

// Removes the file on scope exit.
struct temp_path {
    std::string path;
    explicit temp_path(const char* tag)
        : path((std::filesystem::temp_directory_path()
                / ("hpc_journal_" + std::string(tag) + "_" + std::to_string(::getpid())))
                   .string())
    {}
    ~temp_path()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST(FileJournal, AppendAndRead)
{
    temp_path tmp("basic");
    auto j = file_journal<record>::create(tmp.path, 4);
    EXPECT_EQ(j.capacity(), 4u);
    EXPECT_EQ(j.size(), 0u);

    record r{};
    EXPECT_FALSE(j.try_read(0, r));
    for (std::uint64_t i = 0; i < 4; ++i) EXPECT_TRUE(j.try_append({i, 1.5 * double(i)}));
    EXPECT_FALSE(j.try_append({99, 0})); // full
    EXPECT_EQ(j.size(), 4u);

    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(j.try_read(i, r));
        EXPECT_EQ(r.seq, i);
        EXPECT_EQ(r.value, 1.5 * double(i));
    }
    EXPECT_FALSE(j.try_read(4, r));
    j.flush();
}

TEST(FileJournal, PersistsAndResumesAfterReopen)
{
    temp_path tmp("reopen");
    {
        auto j = file_journal<record>::create(tmp.path, 10);
        for (std::uint64_t i = 0; i < 3; ++i) ASSERT_TRUE(j.try_append({i, 0}));
    }
    auto j = file_journal<record>::open(tmp.path);
    EXPECT_EQ(j.size(), 3u);
    EXPECT_EQ(j.capacity(), 10u);
    ASSERT_TRUE(j.try_append({3, 0}));

    record r{};
    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(j.try_read(i, r));
        EXPECT_EQ(r.seq, i);
    }
}

TEST(FileJournal, CreateTruncatesExisting)
{
    temp_path tmp("trunc");
    {
        auto j = file_journal<record>::create(tmp.path, 4);
        ASSERT_TRUE(j.try_append({1, 0}));
    }
    auto j = file_journal<record>::create(tmp.path, 4);
    EXPECT_EQ(j.size(), 0u);
}

TEST(FileJournal, OpenRejectsMismatchedType)
{
    temp_path tmp("mismatch");
    auto j = file_journal<record>::create(tmp.path, 4);
    EXPECT_THROW(file_journal<std::uint32_t>::open(tmp.path), std::runtime_error);
}

TEST(FileJournal, OpenRejectsForeignFile)
{
    temp_path tmp("foreign");
    std::ofstream(tmp.path) << std::string(1024, 'x');
    EXPECT_THROW(file_journal<record>::open(tmp.path), std::runtime_error);
}

TEST(FileJournal, OpenMissingThrows)
{
    temp_path tmp("missing");
    EXPECT_THROW(file_journal<record>::open(tmp.path), std::system_error);
}

TEST(FileJournal, ReaderTailsWriter)
{
    constexpr std::uint64_t kRecords = 100'000;
    temp_path tmp("tail");
    auto writer = file_journal<record>::create(tmp.path, kRecords);
    auto reader = file_journal<record>::open(tmp.path);

    std::thread t([&] {
        for (std::uint64_t i = 0; i < kRecords; ++i) ASSERT_TRUE(writer.try_append({i, 0}));
    });
    record r{};
    for (std::uint64_t i = 0; i < kRecords;) {
        if (reader.try_read(i, r)) {
            ASSERT_EQ(r.seq, i);
            ++i;
        }
    }
    t.join();
}

// A writer in another process appends and exits; a reader started afterwards
// replays everything from the file.
TEST(FileJournal, CrossProcessReplayAfterWriterExits)
{
    constexpr std::uint64_t kRecords = 50'000;
    temp_path tmp("fork");
    file_journal<record>::create(tmp.path, kRecords);

    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        int rc = 0;
        try {
            auto j = file_journal<record>::open(tmp.path);
            for (std::uint64_t i = 0; i < kRecords; ++i)
                if (!j.try_append({i, double(i)})) rc = 2;
        } catch (...) {
            rc = 1;
        }
        ::_exit(rc);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    auto j = file_journal<record>::open(tmp.path);
    ASSERT_EQ(j.size(), kRecords);
    record r{};
    for (std::uint64_t i = 0; i < kRecords; ++i) {
        ASSERT_TRUE(j.try_read(i, r));
        ASSERT_EQ(r.seq, i);
        ASSERT_EQ(r.value, double(i));
    }
}
