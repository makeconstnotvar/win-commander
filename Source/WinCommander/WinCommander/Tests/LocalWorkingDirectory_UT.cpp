// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Tools/LocalWorkingDirectory.h>

namespace {

using nc::core::LocalWorkingDirectoryRefusal;
using nc::core::PaneLocationFacts;
using nc::core::ResolveLocalWorkingDirectory;

PaneLocationFacts Local(const std::string_view _path)
{
    return {.is_native_filesystem = true, .is_uniform = true, .path = _path};
}

} // namespace

#define PREFIX "nc::core::ResolveLocalWorkingDirectory "

TEST_CASE(PREFIX "refuses a path that only means something inside this application")
{
    // An archive or remote path looks like an ordinary absolute path. Handed to a local shell it
    // resolves against the real filesystem, so the tool would open somewhere the user was not
    // looking - a real but wrong directory, which is worse than declining.
    PaneLocationFacts archive = Local("/Users/me/backup.zip/etc");
    archive.is_native_filesystem = false;
    const auto refused = ResolveLocalWorkingDirectory(archive);
    CHECK_FALSE(refused.Usable());
    CHECK(refused.refusal == LocalWorkingDirectoryRefusal::NotLocalFilesystem);
    CHECK(refused.path.empty());

    // Provider is checked before uniformity, because it is the failure that would otherwise
    // resolve silently rather than visibly.
    PaneLocationFacts remote = archive;
    remote.is_uniform = false;
    CHECK(ResolveLocalWorkingDirectory(remote).refusal == LocalWorkingDirectoryRefusal::NotLocalFilesystem);
}

TEST_CASE(PREFIX "refuses a listing that is not one directory")
{
    // Search results and other non-uniform listings have no single "here" to open.
    PaneLocationFacts facts = Local("/Users/me");
    facts.is_uniform = false;
    const auto result = ResolveLocalWorkingDirectory(facts);
    CHECK(result.refusal == LocalWorkingDirectoryRefusal::NotUniform);
    CHECK(result.path.empty());
}

TEST_CASE(PREFIX "refuses an absent or relative location")
{
    CHECK(ResolveLocalWorkingDirectory(Local("")).refusal == LocalWorkingDirectoryRefusal::NoLocation);
    CHECK(ResolveLocalWorkingDirectory(Local("relative/path")).refusal == LocalWorkingDirectoryRefusal::NoLocation);
}

TEST_CASE(PREFIX "hands over a clean absolute path")
{
    const auto result = ResolveLocalWorkingDirectory(Local("/Users/me/Documents"));
    REQUIRE(result.Usable());
    CHECK(result.path == "/Users/me/Documents");
}

TEST_CASE(PREFIX "normalizes a trailing slash but keeps the root")
{
    // Some tools echo the working directory back, where a doubled separator reads as a bug.
    CHECK(ResolveLocalWorkingDirectory(Local("/Users/me/")).path == "/Users/me");
    CHECK(ResolveLocalWorkingDirectory(Local("/Users/me///")).path == "/Users/me");
    // The root is the one path whose trailing slash is the path.
    CHECK(ResolveLocalWorkingDirectory(Local("/")).path == "/");
    CHECK(ResolveLocalWorkingDirectory(Local("///")).path == "/");
}
