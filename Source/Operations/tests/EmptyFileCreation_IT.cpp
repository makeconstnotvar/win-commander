// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include "Environment.h"
#include <Operations/EmptyFileCreation.h>
#include <VFS/Native.h>
#include <fcntl.h>
#include <unistd.h>

namespace EmptyFileCreationTests {

using namespace nc::ops;

#define PREFIX "Operations::EmptyFileCreation "

TEST_CASE(PREFIX "creates one empty regular file")
{
    const TempTestDir dir;
    const auto host = TestEnv().vfs_native;
    EmptyFileCreation operation{"note.txt", dir.directory.native(), *host};
    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    const auto stat = host->Stat((dir.directory / "note.txt").native(), VFSFlags::F_NoFollow);
    REQUIRE(stat);
    CHECK(stat->mode_bits.reg);
    CHECK(stat->size == 0);
}

TEST_CASE(PREFIX "does not replace an existing file")
{
    const TempTestDir dir;
    const auto host = TestEnv().vfs_native;
    const auto path = dir.directory / "note.txt";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "kept", 4) == 4);
    REQUIRE(::close(fd) == 0);

    EmptyFileCreation operation{"note.txt", dir.directory.native(), *host};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() != OperationState::Completed);
    const auto stat = host->Stat(path.native(), VFSFlags::F_NoFollow);
    REQUIRE(stat);
    CHECK(stat->size == 4);
}

TEST_CASE(PREFIX "rejects nested and parent paths")
{
    const TempTestDir dir;
    const auto host = TestEnv().vfs_native;

    for( const std::string_view name : {"nested/note.txt", "../note.txt", ".", "..", ""} ) {
        EmptyFileCreation operation{std::string{name}, dir.directory.native(), *host};
        operation.Start();
        operation.Wait();
        CHECK(operation.State() != OperationState::Completed);
    }

    CHECK_FALSE(host->Exists((dir.directory / "nested").native()));
    CHECK_FALSE(host->Exists((dir.directory.parent_path() / "note.txt").native()));

    const std::string embedded_nul{"hidden\0suffix", 13};
    EmptyFileCreation embedded_nul_operation{embedded_nul, dir.directory.native(), *host};
    embedded_nul_operation.Start();
    embedded_nul_operation.Wait();
    CHECK(embedded_nul_operation.State() != OperationState::Completed);
    CHECK_FALSE(host->Exists((dir.directory / "hidden").native()));
}

TEST_CASE(PREFIX "reports exactly one processed item")
{
    const TempTestDir dir;
    const auto host = TestEnv().vfs_native;
    EmptyFileCreation operation{"note.txt", dir.directory.native(), *host};
    operation.Start();
    operation.Wait();

    CHECK(operation.Statistics().VolumeTotal(Statistics::SourceType::Items) == 1);
    CHECK(operation.Statistics().VolumeProcessed(Statistics::SourceType::Items) == 1);
}

} // namespace EmptyFileCreationTests

#undef PREFIX
