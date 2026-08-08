// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Tools/OrphanedTemporary.h>

namespace {
using nc::core::IsOrphanedAtomicWriteTemporary;
}

#define PREFIX "nc::core::IsOrphanedAtomicWriteTemporary "

TEST_CASE(PREFIX "recognises what WriteAtomically actually leaves behind")
{
    CHECK(IsOrphanedAtomicWriteTemporary("config.json", ".config.json.nctmp.a1B2c3"));
    CHECK(IsOrphanedAtomicWriteTemporary("config.json", ".config.json.nctmp.ZZZZZZ"));
    CHECK(IsOrphanedAtomicWriteTemporary("config.json", ".config.json.nctmp.000000"));
    CHECK(IsOrphanedAtomicWriteTemporary("no-extension", ".no-extension.nctmp.aB3xY9"));
}

TEST_CASE(PREFIX "never matches a file the user could plausibly own")
{
    // The reason the marker exists. mkstemp's suffix is six alphanumerics, which is exactly the
    // shape of names people choose - both of these would have matched a shape-only rule and been
    // deleted. That was a real defect in this function's first draft, caught by this case.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", "notes.txt.backup"));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", "notes.txt.bak123"));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", "notes.txt.old"));
    // Even a hand-written name that imitates the marker's shape but not the marker itself.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctemp.a1B2c3"));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", "notes.txt.nctmp.a1B2c3")); // no leading dot
}

TEST_CASE(PREFIX "requires the exact suffix shape after the marker")
{
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctmp.a1b2c"));   // five
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctmp.a1b2c34")); // seven
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctmp.a1b2c-"));  // outside the alphabet
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctmp.a1b2c."));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("notes.txt", ".notes.txt.nctmp.a1b2 3"));
}

TEST_CASE(PREFIX "never matches the target itself or a different target's temporary")
{
    // Deleting the target would turn crash cleanup into the data loss it exists to prevent.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("config.json", "config.json"));
    // A temporary belonging to a neighbouring file is not ours to remove either.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("config.json", ".other.json.nctmp.a1B2c3"));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("config.json", ".config.jsonx.nctmp.a1B2c3"));
    // A prefix relationship is not a match: "config" must not claim "config.json"'s temporary.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("config", ".config.json.nctmp.a1B2c3"));
}

TEST_CASE(PREFIX "refuses to answer without a target")
{
    // With no target every candidate would have to be judged on shape alone, which is exactly the
    // guess this function exists to avoid.
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("", "..nctmp.a1B2c3"));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("", ""));
    CHECK_FALSE(IsOrphanedAtomicWriteTemporary("config.json", ""));
}
