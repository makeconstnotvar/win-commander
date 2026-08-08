// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Tools/LocalToolLaunch.h>

#include <string>
#include <vector>

namespace {

using nc::core::LocalToolLaunchRefusal;
using nc::core::LocalToolRole;
using nc::core::PaneLocationFacts;
using nc::core::PrepareLocalToolLaunch;

PaneLocationFacts Local(const std::string_view _path)
{
    return PaneLocationFacts{.is_native_filesystem = true, .is_uniform = true, .path = _path};
}

} // namespace

#define PREFIX "nc::core::PrepareLocalToolLaunch "

TEST_CASE(PREFIX "opens a terminal at the pane's directory and hands it nothing else")
{
    const auto request = PrepareLocalToolLaunch(
        LocalToolRole::Terminal, Local("/Users/me/project/"), "com.apple.Terminal", {"/Users/me/project/file.txt"});
    REQUIRE(request);
    CHECK(request->application == "com.apple.Terminal");
    // Trailing slash stripped by the resolver, which some tools echo back as a doubled separator.
    CHECK(request->working_directory == "/Users/me/project");
    // "Open terminal here" must mean the same thing whatever happens to be selected.
    CHECK(request->documents.empty());
}

TEST_CASE(PREFIX "hands an editor the selection, as separate arguments")
{
    const auto request = PrepareLocalToolLaunch(LocalToolRole::Editor,
                                                Local("/Users/me/project"),
                                                "com.microsoft.VSCode",
                                                {"/Users/me/project/a.txt", "/Users/me/project/sub/b.txt"});
    REQUIRE(request);
    CHECK(request->working_directory == "/Users/me/project");
    CHECK(request->documents ==
          std::vector<std::string>{"/Users/me/project/a.txt", "/Users/me/project/sub/b.txt"});
}

TEST_CASE(PREFIX "checks the location before it looks at the application at all")
{
    // The ordering is the decision. A missing editor is an obvious failure the user can fix; a path
    // inside an archive resolves silently against the real filesystem and opens the wrong place, so
    // that is the refusal that has to be reached.
    const PaneLocationFacts in_archive{
        .is_native_filesystem = false, .is_uniform = true, .path = "/Users/me/backup.zip/etc"};

    const auto with_no_app = PrepareLocalToolLaunch(LocalToolRole::Terminal, in_archive, "");
    REQUIRE_FALSE(with_no_app);
    CHECK(with_no_app.error() == LocalToolLaunchRefusal::LocationUnusable);

    const auto with_app = PrepareLocalToolLaunch(LocalToolRole::Terminal, in_archive, "com.apple.Terminal");
    REQUIRE_FALSE(with_app);
    CHECK(with_app.error() == LocalToolLaunchRefusal::LocationUnusable);
}

TEST_CASE(PREFIX "refuses a listing that has no single directory")
{
    // Search results have no "here" to open.
    const PaneLocationFacts non_uniform{
        .is_native_filesystem = true, .is_uniform = false, .path = "/Users/me/project"};
    const auto request = PrepareLocalToolLaunch(LocalToolRole::Terminal, non_uniform, "com.apple.Terminal");
    REQUIRE_FALSE(request);
    CHECK(request.error() == LocalToolLaunchRefusal::LocationUnusable);
}

TEST_CASE(PREFIX "tells an unconfigured application apart from an unusable one")
{
    // Nothing chosen yet is a settings problem; whitespace is a broken value. Both refuse, but a
    // surface that reported them the same way would send the user to the wrong place to fix it.
    const auto unconfigured = PrepareLocalToolLaunch(LocalToolRole::Editor, Local("/Users/me"), "");
    REQUIRE_FALSE(unconfigured);
    CHECK(unconfigured.error() == LocalToolLaunchRefusal::ApplicationNotConfigured);

    const auto blank = PrepareLocalToolLaunch(LocalToolRole::Editor, Local("/Users/me"), "   ");
    REQUIRE_FALSE(blank);
    CHECK(blank.error() == LocalToolLaunchRefusal::ApplicationUnusable);
}

TEST_CASE(PREFIX "drops a selection that no longer belongs to this directory")
{
    // A selection can outlive the listing it came from. Handing an editor a path from somewhere else
    // is how a stale selection ends up editing the wrong file.
    const auto request = PrepareLocalToolLaunch(LocalToolRole::Editor,
                                                Local("/Users/me/project"),
                                                "com.microsoft.VSCode",
                                                {"/Users/me/project/keep.txt",
                                                 "/Users/me/elsewhere/drop.txt",
                                                 "relative.txt",
                                                 "",
                                                 "/Users/me/project-other/drop.txt"});
    REQUIRE(request);
    // `/Users/me/project-other` is not inside `/Users/me/project`, however alike the two look as
    // strings - the same component-versus-prefix rule the mount table needs.
    CHECK(request->documents == std::vector<std::string>{"/Users/me/project/keep.txt"});
}

TEST_CASE(PREFIX "keeps a path that is full of shell syntax intact")
{
    // These reach the platform as arguments, never as a command line, so a filename stays a filename
    // rather than becoming a sequence of operators.
    const std::string awkward = "/Users/me/project/a file; rm -rf $x 'quoted'\nand a newline.txt";
    const auto request =
        PrepareLocalToolLaunch(LocalToolRole::Editor, Local("/Users/me/project"), "com.apple.TextEdit", {awkward});
    REQUIRE(request);
    REQUIRE(request->documents.size() == 1);
    CHECK(request->documents.front() == awkward);
}

TEST_CASE(PREFIX "works at the root, where every path is inside the directory")
{
    const auto request = PrepareLocalToolLaunch(
        LocalToolRole::Editor, Local("/"), "com.apple.TextEdit", {"/etc/hosts", "relative"});
    REQUIRE(request);
    CHECK(request->working_directory == "/");
    // The root's trailing separator must not make `/etc/hosts` read as outside it.
    CHECK(request->documents == std::vector<std::string>{"/etc/hosts"});
}

#undef PREFIX
