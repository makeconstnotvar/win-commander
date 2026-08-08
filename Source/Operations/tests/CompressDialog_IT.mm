// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include "../source/Compression/ArchiveCreationFormat.h"
#include "../source/Compression/CompressDialog.h"

#include <VFS/VFS.h>
#include <VFS/Native.h>

#include <filesystem>
#include <fstream>
#include <span>

/**
 * The picker's action is private to the dialog. Declaring it here rather than reaching through
 * `performSelector` untyped keeps the test honest about what it is driving: the same message the
 * pop-up sends when a user changes the selection.
 */
@interface NCOpsCompressDialog (CompressDialogTesting)
- (IBAction)onFormatChanged:(id)_sender;
@end

namespace CompressDialogTests {

using namespace nc;
using namespace nc::ops;

#define PREFIX "Operations::CompressDialog "

static std::vector<VFSListingItem>
FetchItems(const std::string &_directory_path, const std::vector<std::string> &_filenames, VFSHost &_host)
{
    return _host.FetchFlexibleListingItems(_directory_path, _filenames, 0).value_or(std::vector<VFSListingItem>{});
}

TEST_CASE(PREFIX "offers exactly the formats the engine can produce")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    std::ofstream(tmp_dir.directory / "item") << "payload";

    NCOpsCompressDialog *const dialog =
        [[NCOpsCompressDialog alloc] initWithItems:FetchItems(tmp_dir.directory, {"item"}, *native_host)
                                    destinationVFS:native_host
                                initialDestination:tmp_dir.directory.native()];
    REQUIRE(dialog.window != nil); // forces the nib to load

    NSPopUpButton *const popup = [dialog valueForKey:@"formatPopUp"];
    // A mistyped outlet would leave this nil, every menu operation would quietly do nothing, and the
    // dialog would ship with an empty picker. Worth asserting rather than assuming.
    REQUIRE(popup != nil);

    const std::span<const ArchiveCreationFormatInfo> formats = SupportedArchiveCreationFormats();
    REQUIRE(popup.numberOfItems == static_cast<NSInteger>(formats.size()));
    for( size_t i = 0; i < formats.size(); ++i ) {
        NSMenuItem *const item = [popup itemAtIndex:static_cast<NSInteger>(i)];
        CHECK(item.title.UTF8String == formats[i].extension);
        CHECK(item.tag == static_cast<NSInteger>(formats[i].format));
    }
    CHECK(dialog.format == ArchiveCreationFormat::Zip);
}

TEST_CASE(PREFIX "withdraws password protection when the format cannot carry it")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    std::ofstream(tmp_dir.directory / "item") << "payload";

    NCOpsCompressDialog *const dialog =
        [[NCOpsCompressDialog alloc] initWithItems:FetchItems(tmp_dir.directory, {"item"}, *native_host)
                                    destinationVFS:native_host
                                initialDestination:tmp_dir.directory.native()];
    REQUIRE(dialog.window != nil);

    NSPopUpButton *const popup = [dialog valueForKey:@"formatPopUp"];
    NSButton *const checkbox = [dialog valueForKey:@"protectWithPasswordCheckbox"];
    REQUIRE(popup != nil);
    REQUIRE(checkbox != nil);

    [dialog setValue:@YES forKey:@"protectWithPassword"];
    [dialog setValue:@"secret" forKey:@"passwordString"];
    CHECK(checkbox.enabled);

    // Switching to a format that cannot encrypt withdraws the request rather than leaving a ticked
    // box that would produce an unprotected archive the user believes is protected.
    REQUIRE([popup selectItemWithTag:static_cast<NSInteger>(ArchiveCreationFormat::Tar)]);
    [dialog onFormatChanged:popup];
    CHECK(dialog.format == ArchiveCreationFormat::Tar);
    CHECK_FALSE(checkbox.enabled);
    CHECK_FALSE([[dialog valueForKey:@"protectWithPassword"] boolValue]);

    // What was typed survives, so coming back to a format that can encrypt does not ask for it again.
    REQUIRE([popup selectItemWithTag:static_cast<NSInteger>(ArchiveCreationFormat::Zip)]);
    [dialog onFormatChanged:popup];
    CHECK(checkbox.enabled);
    CHECK([[dialog valueForKey:@"passwordString"] isEqualToString:@"secret"]);
    // But it is not silently re-engaged: asking for protection again is the user's call.
    CHECK_FALSE([[dialog valueForKey:@"protectWithPassword"] boolValue]);
}

} // namespace CompressDialogTests

#undef PREFIX
