// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Tools/OrphanedTemporary.h>

#include <filesystem>
#include <fstream>
#include <string>

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

#undef PREFIX
#define PREFIX "nc::core::SweepOrphanedTemporaries "

namespace {

using nc::core::SweepOrphanedTemporaries;

void Touch(const std::filesystem::path &_path, const std::string_view _contents = "x")
{
    std::ofstream out{_path};
    REQUIRE(out.is_open());
    out << _contents;
}

} // namespace

TEST_CASE(PREFIX "removes leftovers and reports exactly what it removed")
{
    const TempTestDir dir;
    const std::filesystem::path target = dir.directory / "config.json";
    Touch(target, "live");
    Touch(dir.directory / ".config.json.nctmp.a1B2c3");
    Touch(dir.directory / ".config.json.nctmp.ZZZZZZ");

    const auto result = SweepOrphanedTemporaries(target);
    CHECK(result.removed.size() == 2);
    CHECK(result.failed.empty());
    CHECK(std::filesystem::exists(target));
    CHECK_FALSE(std::filesystem::exists(dir.directory / ".config.json.nctmp.a1B2c3"));
    CHECK_FALSE(std::filesystem::exists(dir.directory / ".config.json.nctmp.ZZZZZZ"));
}

TEST_CASE(PREFIX "leaves every file it has no claim over")
{
    const TempTestDir dir;
    const std::filesystem::path target = dir.directory / "notes.txt";
    Touch(target, "live");
    // Names a shape-only rule would have destroyed, plus a neighbour's leftover.
    Touch(dir.directory / "notes.txt.backup");
    Touch(dir.directory / "notes.txt.bak123");
    Touch(dir.directory / ".other.txt.nctmp.a1B2c3");
    Touch(dir.directory / "unrelated.dat");

    const auto result = SweepOrphanedTemporaries(target);
    CHECK(result.removed.empty());
    CHECK(result.failed.empty());
    for( const auto *name : {"notes.txt", "notes.txt.backup", "notes.txt.bak123",
                             ".other.txt.nctmp.a1B2c3", "unrelated.dat"} ) {
        INFO(name);
        CHECK(std::filesystem::exists(dir.directory / name));
    }
}

TEST_CASE(PREFIX "will not follow a symlink wearing the name")
{
    // A planted link would otherwise redirect the deletion to a file outside the directory.
    const TempTestDir dir;
    const std::filesystem::path target = dir.directory / "config.json";
    Touch(target, "live");
    const std::filesystem::path victim = dir.directory / "precious.dat";
    Touch(victim, "must survive");

    std::error_code ec;
    std::filesystem::create_symlink(victim, dir.directory / ".config.json.nctmp.a1B2c3", ec);
    REQUIRE_FALSE(ec);

    const auto result = SweepOrphanedTemporaries(target);
    CHECK(result.removed.empty());
    CHECK(std::filesystem::exists(victim));
    CHECK(std::filesystem::is_symlink(dir.directory / ".config.json.nctmp.a1B2c3"));
}

TEST_CASE(PREFIX "does not recurse, and tolerates a directory bearing the name")
{
    const TempTestDir dir;
    const std::filesystem::path target = dir.directory / "config.json";
    Touch(target, "live");
    // A leftover is created beside its target, so a nested one is not this sweep's business.
    std::filesystem::create_directory(dir.directory / "nested");
    Touch(dir.directory / "nested" / ".config.json.nctmp.a1B2c3");
    // A directory wearing the name was not written by us either.
    std::filesystem::create_directory(dir.directory / ".config.json.nctmp.ZZZZZZ");

    const auto result = SweepOrphanedTemporaries(target);
    CHECK(result.removed.empty());
    CHECK(std::filesystem::exists(dir.directory / "nested" / ".config.json.nctmp.a1B2c3"));
    CHECK(std::filesystem::is_directory(dir.directory / ".config.json.nctmp.ZZZZZZ"));
}

TEST_CASE(PREFIX "answers safely for a path it cannot act on")
{
    const TempTestDir dir;
    CHECK(SweepOrphanedTemporaries({}) == nc::core::OrphanedTemporarySweepResult{});
    CHECK(SweepOrphanedTemporaries(dir.directory / "never-existed.json") ==
          nc::core::OrphanedTemporarySweepResult{});
    // A directory that does not exist is not an error, just nothing to sweep.
    CHECK(SweepOrphanedTemporaries("/nonexistent-directory-xyz/file.json") ==
          nc::core::OrphanedTemporarySweepResult{});
}
