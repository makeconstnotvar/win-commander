// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Panel/PanelViewFieldEditor.h>
#include <VFS/VFSListingInput.h>
#include "Tests.h"

#define PREFIX "PanelViewFieldEditor "

namespace {

VFSListingItem TestItem()
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.filenames.emplace_back("original.txt");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR | S_IWUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input))->Item(0);
}

TEST_CASE(PREFIX "rejected submission remains active and accepted submission finishes once")
{
    @autoreleasepool {
        NCPanelViewFieldEditor *const editor = [[NCPanelViewFieldEditor alloc] initWithItem:TestItem()];
        __block int submissions = 0;
        __block int completions = 0;
        __block bool accept = false;
        editor.onTextEntered = ^bool(const std::string &) {
          ++submissions;
          return accept;
        };
        editor.onEditingFinished = ^{
          ++completions;
        };

        editor.editor.string = @"candidate.txt";
        CHECK([editor textView:editor.editor doCommandBySelector:@selector(insertNewline:)]);
        CHECK(submissions == 1);
        CHECK(completions == 0);
        CHECK(editor.onTextEntered != nil);
        CHECK(editor.onEditingFinished != nil);

        accept = true;
        CHECK([editor textView:editor.editor doCommandBySelector:@selector(insertNewline:)]);
        CHECK(submissions == 2);
        CHECK(completions == 1);
        CHECK(editor.onTextEntered == nil);
        CHECK(editor.onEditingFinished == nil);
    }
}

TEST_CASE(PREFIX "empty name reaches validation and can be corrected")
{
    @autoreleasepool {
        NCPanelViewFieldEditor *const editor = [[NCPanelViewFieldEditor alloc] initWithItem:TestItem()];
        __block std::vector<std::string> submissions;
        __block int completions = 0;
        editor.onTextEntered = ^bool(const std::string &name) {
          submissions.emplace_back(name);
          return !name.empty();
        };
        editor.onEditingFinished = ^{
          ++completions;
        };

        editor.editor.string = @"";
        CHECK([editor textView:editor.editor doCommandBySelector:@selector(insertTab:)]);
        REQUIRE(submissions.size() == 1);
        CHECK(submissions.front().empty());
        CHECK(completions == 0);

        editor.editor.string = @"fixed.txt";
        CHECK([editor textView:editor.editor doCommandBySelector:@selector(insertTab:)]);
        REQUIRE(submissions.size() == 2);
        CHECK(submissions.back() == "fixed.txt");
        CHECK(completions == 1);
    }
}

TEST_CASE(PREFIX "escape cancels without submitting")
{
    @autoreleasepool {
        NCPanelViewFieldEditor *const editor = [[NCPanelViewFieldEditor alloc] initWithItem:TestItem()];
        __block int submissions = 0;
        __block int completions = 0;
        editor.onTextEntered = ^bool(const std::string &) {
          ++submissions;
          return true;
        };
        editor.onEditingFinished = ^{
          ++completions;
        };

        CHECK([editor textView:editor.editor doCommandBySelector:@selector(cancelOperation:)]);
        CHECK(submissions == 0);
        CHECK(completions == 1);
        CHECK(editor.onTextEntered == nil);
        CHECK(editor.onEditingFinished == nil);
    }
}

TEST_CASE(PREFIX "editor text view exposes an accessibility identifier and label")
{
    @autoreleasepool {
        NCPanelViewFieldEditor *const editor = [[NCPanelViewFieldEditor alloc] initWithItem:TestItem()];
        CHECK([editor.editor.accessibilityIdentifier isEqualToString:@"wincommander.panel.renameField"]);
        CHECK(editor.editor.accessibilityLabel.length > 0);
    }
}

} // namespace

#undef PREFIX
