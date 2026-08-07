// Copyright (C) 2013-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ContextMenu.h"
#include "Actions/Compress.h"
#include "Actions/CopyFilePaths.h"
#include "Actions/CopyToPasteboard.h"
#include "Actions/Delete.h"
#include "Actions/Duplicate.h"
#include "NCPanelOpenWithMenuDelegate.h"
#include "PanelController.h"
#include "PanelControllerActionsDispatcher.h"
#include "PanelView.h"
#include <Panel/TagsStorage.h>
#include <Panel/UI/TagsPresentation.h>
#include <Utility/ObjCpp.h>
#include <Utility/StringExtras.h>
#include <VFS/VFS.h>
#include <algorithm>
#include <memory>
#include <pstld/pstld.h>
#include <ranges>
#include <tuple>

// TODO: remove this global dependency
#include <WinCommander/Bootstrap/AppDelegate.h>

#include <WinCommander/Core/AnyHolder.h>
#include <WinCommander/States/CommandPresentationAdapter.h>

using namespace nc::panel;

@interface NCPanelContextMenuSharingDelegate : NSObject <NSSharingServiceDelegate>
@property(nonatomic, weak) NSWindow *sourceWindow;
@end

@implementation NCPanelContextMenu {
    std::vector<VFSListingItem> m_Items;
    PanelController *m_Panel;
    NSMutableArray *m_ShareItemsURLs;
    NCPanelOpenWithMenuDelegate *m_OpenWithDelegate;
    std::unique_ptr<actions::PanelAction> m_CopyPathnameAction;
    std::unique_ptr<actions::PanelAction> m_DuplicateAction;
    std::unique_ptr<actions::PanelAction> m_CompressHereAction;
    std::unique_ptr<actions::PanelAction> m_CompressToOppositeAction;
}

- (instancetype)initForBackgroundOfPanel:(PanelController *)_panel
{
    if( !_panel )
        return nil;
    self = [super init];
    if( self ) {
        m_Panel = _panel;
        self.delegate = self;
        self.minimumWidth = 230;
        [self doBackgroundStuffing];
    }
    return self;
}

- (instancetype)initWithItems:(std::vector<VFSListingItem>)_items
                      ofPanel:(PanelController *)_panel
               withFileOpener:(nc::panel::FileOpener &)_file_opener
                    withUTIDB:(const nc::utility::UTIDB &)_uti_db
{
    if( _items.empty() )
        throw std::invalid_argument("NCPanelContextMenu.initWithData - there's no items");
    self = [super init];
    if( self ) {
        m_Panel = _panel;
        m_Items = std::move(_items);

        self.delegate = self;
        self.minimumWidth = 230; // hardcoding is bad!
        auto &global_config = NCAppDelegate.me.globalConfig;

        m_CopyPathnameAction = std::make_unique<actions::context::CopyPathname>(m_Items);
        m_DuplicateAction = std::make_unique<actions::context::Duplicate>(global_config, m_Items);
        m_CompressHereAction = std::make_unique<actions::context::CompressHere>(global_config, m_Items);
        m_CompressToOppositeAction = std::make_unique<actions::context::CompressToOpposite>(global_config, m_Items);
        m_OpenWithDelegate = [[NCPanelOpenWithMenuDelegate alloc] initWithFileOpener:_file_opener utiDB:_uti_db];
        [m_OpenWithDelegate setContextSource:m_Items];
        m_OpenWithDelegate.target = m_Panel;

        [self doStuffing];
    }
    return self;
}

- (void)doBackgroundStuffing
{
    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
    const auto add_registry_item = [&](NSString *const _title,
                                       const SEL _selector,
                                       const nc::core::CommandState &_state) {
        NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:_title action:nil keyEquivalent:@""];
        const bool enabled = nc::presentation::CommandPresentationAdapter::Apply(_state, item);
        item.enabled = enabled;
        if( enabled ) {
            item.target = self;
            item.action = _selector;
        }
        if( !item.hidden )
            [self addItem:item];
    };

    add_registry_item(NSLocalizedString(@"commands.file.paste.title", "Paste command title"),
                      @selector(OnBackgroundPaste:),
                      [dispatcher filePasteCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
    add_registry_item(
        NSLocalizedString(@"commands.file.newFolder.title", "New Folder command title"),
        @selector(OnBackgroundNewFolder:),
        [dispatcher fileNewFolderCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
    [self addItem:NSMenuItem.separatorItem];

    add_registry_item(
        NSLocalizedString(@"commands.pane.selectAll.title", "Select All command title"),
        @selector(OnBackgroundSelectAll:),
        [dispatcher paneSelectAllCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
    add_registry_item(
        NSLocalizedString(@"commands.pane.invertSelection.title", "Invert Selection command title"),
        @selector(OnBackgroundInvertSelection:),
        [dispatcher paneInvertSelectionCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
    [self addItem:NSMenuItem.separatorItem];

    add_registry_item(
        NSLocalizedString(@"commands.view.toggleHiddenFiles.title", "Show hidden files command title"),
        @selector(OnBackgroundToggleHiddenFiles:),
        [dispatcher viewToggleHiddenFilesCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
    add_registry_item(
        NSLocalizedString(@"commands.navigation.refresh.title", "Refresh command title"),
        @selector(OnBackgroundRefresh:),
        [dispatcher navigationRefreshCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu]);
}

- (void)menuDidClose:(NSMenu *)menu
{
    [m_Panel contextMenuDidClose:menu];
}

- (void)doStuffing
{
    //////////////////////////////////////////////////////////////////////
    // regular Open item
    const auto open_item = [NSMenuItem new];
    open_item.title = NSLocalizedStringFromTable(
        @"Open", @"FilePanelsContextMenu", "Menu item title for opening a file by default, for English is 'Open'");
    open_item.target = self;
    open_item.action = @selector(OnRegularOpen:);
    [self addItem:open_item];

    //////////////////////////////////////////////////////////////////////
    // Open With... stuff
    {
        NSMenu *openwith_submenu = [NSMenu new];
        openwith_submenu.identifier = NCPanelOpenWithMenuDelegate.regularMenuIdentifier;
        openwith_submenu.delegate = m_OpenWithDelegate;
        [m_OpenWithDelegate addManagedMenu:openwith_submenu];

        NSMenuItem *openwith = [NSMenuItem new];
        openwith.title =
            NSLocalizedStringFromTable(@"Open With",
                                       @"FilePanelsContextMenu",
                                       "Submenu title to choose app to open with, for English is 'Open With'");
        openwith.submenu = openwith_submenu;
        openwith.keyEquivalent = @"";
        [self addItem:openwith];

        NSMenu *always_openwith_submenu = [NSMenu new];
        always_openwith_submenu.identifier = NCPanelOpenWithMenuDelegate.alwaysOpenWithMenuIdentifier;
        always_openwith_submenu.delegate = m_OpenWithDelegate;
        [m_OpenWithDelegate addManagedMenu:always_openwith_submenu];

        NSMenuItem *always_openwith = [NSMenuItem new];
        always_openwith.title = NSLocalizedStringFromTable(
            @"Always Open With",
            @"FilePanelsContextMenu",
            "Submenu title to choose app to always open with, for English is 'Always Open With'");
        always_openwith.submenu = always_openwith_submenu;
        always_openwith.alternate = true;
        always_openwith.keyEquivalent = @"";
        always_openwith.keyEquivalentModifierMask = NSEventModifierFlagOption;
        [self addItem:always_openwith];

        NSMenuItem *const preview_item = [NSMenuItem new];
        preview_item.title = NSLocalizedString(@"commands.file.preview.title", "Preview command title");
        preview_item.target = self;
        preview_item.action = @selector(OnPreview:);
        preview_item.keyEquivalent = @"";
        const auto preview_state = [m_Panel.view.actionsDispatcher
            filePreviewCommandStateForItems:m_Items
                                      source:nc::core::CommandInvocationSource::ContextMenu];
        [[maybe_unused]] const bool preview_applied =
            nc::presentation::CommandPresentationAdapter::Apply(preview_state, preview_item);
        [self addItem:preview_item];

        if( m_Items.size() == 1 ) {
            NSMenuItem *const rename_item = [NSMenuItem new];
            rename_item.title = NSLocalizedString(@"commands.file.rename.title", "Rename command title");
            rename_item.target = self;
            rename_item.action = @selector(OnRename:);
            rename_item.keyEquivalent = @"";
            [self addItem:rename_item];
        }

        NSMenuItem *const get_info_item = [NSMenuItem new];
        get_info_item.title = NSLocalizedString(@"commands.file.getInfo.title", "Get Info command title");
        get_info_item.target = self;
        get_info_item.action = @selector(OnGetInfo:);
        get_info_item.keyEquivalent = @"";
        const auto get_info_state = [m_Panel.view.actionsDispatcher
            fileGetInfoCommandStateForItems:m_Items
                                      source:nc::core::CommandInvocationSource::ContextMenu];
        [[maybe_unused]] const bool get_info_applied =
            nc::presentation::CommandPresentationAdapter::Apply(get_info_state, get_info_item);
        [self addItem:get_info_item];

        [self addItem:NSMenuItem.separatorItem];
    }

    //////////////////////////////////////////////////////////////////////
    // Move to Trash / Delete Permanently stuff
    const auto trash_item = [NSMenuItem new];
    trash_item.title = NSLocalizedStringFromTable(
        @"Move to Trash", @"FilePanelsContextMenu", "Menu item title to move to trash, for English is 'Move to Trash'");
    trash_item.target = self;
    trash_item.action = @selector(OnMoveToTrash:);
    const auto trash_state = [m_Panel.view.actionsDispatcher
        fileTrashCommandStateForItems:m_Items
                               source:nc::core::CommandInvocationSource::ContextMenu];
    const bool trash_enabled = nc::presentation::CommandPresentationAdapter::Apply(trash_state, trash_item);
    trash_item.keyEquivalent = @"";
    [self addItem:trash_item];

    const auto delete_item = [NSMenuItem new];
    delete_item.title =
        NSLocalizedStringFromTable(@"Delete Permanently",
                                   @"FilePanelsContextMenu",
                                   "Menu item title to delete file, for English is 'Delete Permanently'");
    delete_item.target = self;
    delete_item.action = @selector(OnDeletePermanently:);
    delete_item.alternate = trash_enabled;
    delete_item.keyEquivalent = @"";
    delete_item.keyEquivalentModifierMask = trash_enabled ? NSEventModifierFlagOption : 0;
    [self addItem:delete_item];

    [self addItem:NSMenuItem.separatorItem];

    //////////////////////////////////////////////////////////////////////
    // Compression stuff
    const auto compress_here_item = [NSMenuItem new];
    compress_here_item.title =
        NSLocalizedStringFromTable(@"Compress", @"FilePanelsContextMenu", "Compress some items here");
    compress_here_item.target = self;
    compress_here_item.action = @selector(OnCompressToCurrentPanel:);
    compress_here_item.keyEquivalent = @"";
    [self addItem:compress_here_item];

    const auto compress_in_opposite_item = [NSMenuItem new];
    compress_in_opposite_item.title =
        NSLocalizedStringFromTable(@"Compress in Opposite Panel", @"FilePanelsContextMenu", "Compress some items");
    compress_in_opposite_item.target = self;
    compress_in_opposite_item.action = @selector(OnCompressToOppositePanel:);
    compress_in_opposite_item.keyEquivalent = @"";
    compress_in_opposite_item.alternate = YES;
    compress_in_opposite_item.keyEquivalentModifierMask = NSEventModifierFlagOption;
    [self addItem:compress_in_opposite_item];

    const auto extract_here_item = [NSMenuItem new];
    extract_here_item.title =
        NSLocalizedString(@"commands.archive.extract.title", "Extract archive command title");
    extract_here_item.target = self;
    extract_here_item.action = @selector(OnExtractArchiveHere:);
    extract_here_item.keyEquivalent = @"";
    [self addItem:extract_here_item];

    //////////////////////////////////////////////////////////////////////
    // Duplicate stuff
    const auto duplicate_item = [NSMenuItem new];
    duplicate_item.title = NSLocalizedStringFromTable(@"Duplicate", @"FilePanelsContextMenu", "Duplicate an item");
    duplicate_item.target = self;
    duplicate_item.action = @selector(OnDuplicateItem:);
    [self addItem:duplicate_item];

    const auto calculate_sizes_item = [NSMenuItem new];
    calculate_sizes_item.title =
        NSLocalizedString(@"commands.file.calculateSizes.title", "Calculate directory sizes command title");
    calculate_sizes_item.target = self;
    calculate_sizes_item.action = @selector(OnCalculateSizes:);
    [self addItem:calculate_sizes_item];

    const auto batch_rename_item = [NSMenuItem new];
    batch_rename_item.title = NSLocalizedString(@"commands.file.batchRename.title", "Batch rename command title");
    batch_rename_item.target = self;
    batch_rename_item.action = @selector(OnBatchRename:);
    [self addItem:batch_rename_item];

    //////////////////////////////////////////////////////////////////////
    // Share stuff
    {
        const auto share_submenu = [NSMenu new];
        const auto eligible = std::ranges::all_of(m_Items, [](const auto &_i) { return _i.Host()->IsNativeFS(); });
        if( eligible ) {
            m_ShareItemsURLs = [NSMutableArray new];
            for( auto &i : m_Items )
                if( NSString *s = [NSString stringWithUTF8StdString:i.Path()] )
                    if( NSURL *url = [[NSURL alloc] initFileURLWithPath:s] )
                        [m_ShareItemsURLs addObject:url];

            auto services = [NSSharingService sharingServicesForItems:m_ShareItemsURLs];
            for( NSSharingService *service in services ) {
                NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:service.title
                                                              action:@selector(OnShareWithService:)
                                                       keyEquivalent:@""];
                item.image = service.image;
                item.representedObject = service;
                item.target = self;
                [share_submenu addItem:item];
            }
        }

        const auto share_menuitem = [NSMenuItem new];
        share_menuitem.title = NSLocalizedStringFromTable(@"Share", @"FilePanelsContextMenu", "Share submenu title");
        share_menuitem.submenu = share_submenu;
        share_menuitem.enabled = share_submenu.numberOfItems > 0;
        [self addItem:share_menuitem];
    }

    [self addItem:NSMenuItem.separatorItem];

    //////////////////////////////////////////////////////////////////////
    // Tags stuff
    if( const auto eligible = std::ranges::all_of(m_Items, [](const auto &_i) { return _i.Host()->IsNativeFS(); });
        eligible && NCAppDelegate.me.globalConfig.GetBool("filePanel.FinderTags.enable") ) {
        const std::vector<nc::utility::Tags::Tag> all_tags = NCAppDelegate.me.tagsStorage.Get();
        auto tag_state = [&](const nc::utility::Tags::Tag &_tag) -> NSControlStateValue {
            const auto count = std::ranges::count_if(m_Items, [&](const VFSListingItem &_item) -> bool {
                auto item_tags = _item.Tags();
                return std::ranges::find(item_tags, _tag) != item_tags.end();
            });
            if( count == 0 )
                return NSControlStateValueOff;
            else if( static_cast<size_t>(count) == m_Items.size() )
                return NSControlStateValueOn;
            else
                return NSControlStateValueMixed;
        };
        const auto tags_submenu = [NSMenu new];
        // TODO: that's O(N*M) complexity, might backfire when there's many tags used
        for( auto &tag : all_tags ) {
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8StdString:tag.Label()]
                                                          action:@selector(onTagItem:)
                                                   keyEquivalent:@""];
            item.image = TagsMenuDisplay::Images().at(std::to_underlying(tag.Color()));
            item.state = tag_state(tag);
            item.representedObject = [[AnyHolder alloc] initWithAny:tag];
            item.target = self;
            [tags_submenu addItem:item];
        }

        const auto tags_menuitem = [NSMenuItem new];
        tags_menuitem.title = NSLocalizedStringFromTable(@"Tags", @"FilePanelsContextMenu", "Tags submenu title");
        tags_menuitem.submenu = tags_submenu;
        tags_menuitem.enabled = tags_submenu.numberOfItems > 0;
        [self addItem:tags_menuitem];
        [self addItem:NSMenuItem.separatorItem];
    }

    //////////////////////////////////////////////////////////////////////
    // Cut and Copy elements for native FS.
    {
        NSMenuItem *item = [NSMenuItem new];
        item.target = self;
        item.action = @selector(OnCut:);
        [self addItem:item];

        item = [NSMenuItem new];
        item.target = self;
        item.action = @selector(OnCopyPaths:);
        [self addItem:item];

        item = [NSMenuItem new];
        item.target = self;
        item.action = @selector(OnCopyPathname:);
        item.alternate = true;
        item.keyEquivalent = @"";
        item.keyEquivalentModifierMask = NSEventModifierFlagOption;
        [self addItem:item];
    }

    [self addItem:NSMenuItem.separatorItem];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
    if( item.action == @selector(OnBackgroundPaste:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher filePasteCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnBackgroundNewFolder:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher fileNewFolderCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnBackgroundSelectAll:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher paneSelectAllCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnBackgroundInvertSelection:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher paneInvertSelectionCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnBackgroundToggleHiddenFiles:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher viewToggleHiddenFilesCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnBackgroundRefresh:) )
        return nc::presentation::CommandPresentationAdapter::Apply(
            [dispatcher navigationRefreshCommandStateFromSource:nc::core::CommandInvocationSource::ContextMenu], item);
    if( item.action == @selector(OnCut:) ) {
        actions::UpdateCutToPasteboardMenuItemTitle(m_Items, item);
        const auto state = [m_Panel.view.actionsDispatcher
            fileCutCommandStateForItems:m_Items
                                 source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnCopyPaths:) ) {
        actions::UpdateCopyToPasteboardMenuItemTitle(m_Items, item);
        const auto state = [m_Panel.view.actionsDispatcher
            fileCopyCommandStateForItems:m_Items
                                  source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnRename:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileRenameCommandStateForItems:m_Items
                                     source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnPreview:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            filePreviewCommandStateForItems:m_Items
                                      source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnGetInfo:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileGetInfoCommandStateForItems:m_Items
                                      source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnCopyPathname:) ) {
        std::ignore = m_CopyPathnameAction->ValidateMenuItem(m_Panel, item);
        const auto state = [m_Panel.view.actionsDispatcher
            fileCopyPathCommandStateForItems:m_Items
                                      source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnMoveToTrash:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileTrashCommandStateForItems:m_Items
                                   source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnDeletePermanently:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileDeleteCommandStateForItems:m_Items
                                    source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnDuplicateItem:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileDuplicateCommandStateForItems:m_Items
                                       source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnCalculateSizes:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileCalculateSizesCommandStateForItems:m_Items
                                             source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnBatchRename:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileBatchRenameCommandStateForItems:m_Items
                                          source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnCompressToCurrentPanel:) ) {
        std::ignore = m_CompressHereAction->ValidateMenuItem(m_Panel, item);
        const auto state = [m_Panel.view.actionsDispatcher
            archiveCreateCommandStateForItems:m_Items
                                        source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnExtractArchiveHere:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            archiveExtractCommandStateForItems:m_Items
                                         source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }
    if( item.action == @selector(OnCompressToOppositePanel:) )
        return m_CompressToOppositeAction->ValidateMenuItem(m_Panel, item);
    if( item.action == @selector(OnRegularOpen:) ) {
        const auto state = [m_Panel.view.actionsDispatcher
            fileOpenCommandStateForItems:m_Items
                                  source:nc::core::CommandInvocationSource::ContextMenu];
        return nc::presentation::CommandPresentationAdapter::Apply(state, item);
    }

    return true;
}

- (void)OnBackgroundPaste:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executeFilePasteCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                   sender:sender];
}

- (void)OnBackgroundNewFolder:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executeFileNewFolderCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                       sender:sender];
}

- (void)OnBackgroundSelectAll:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executePaneSelectAllCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                      sender:sender];
}

- (void)OnBackgroundInvertSelection:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executePaneInvertSelectionCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                             sender:sender];
}

- (void)OnBackgroundToggleHiddenFiles:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executeViewToggleHiddenFilesCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                               sender:sender];
}

- (void)OnBackgroundRefresh:(id)sender
{
    [m_Panel.view.actionsDispatcher
        executeNavigationRefreshCommandFromSource:nc::core::CommandInvocationSource::ContextMenu
                                           sender:sender];
}

- (void)OnRegularOpen:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileOpenCommandWithItems:m_Items
                                                             source:nc::core::CommandInvocationSource::ContextMenu
                                                             sender:sender];
}

- (void)OnMoveToTrash:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileTrashCommandWithItems:m_Items
                                                               source:nc::core::CommandInvocationSource::ContextMenu
                                                               sender:sender];
}

- (void)OnDeletePermanently:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileDeleteCommandWithItems:m_Items
                                                                source:nc::core::CommandInvocationSource::ContextMenu
                                                                sender:sender];
}

- (void)OnCopyPaths:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileCopyCommandWithItems:m_Items
                                                             source:nc::core::CommandInvocationSource::ContextMenu
                                                             sender:sender];
}

- (void)OnCut:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileCutCommandWithItems:m_Items
                                                            source:nc::core::CommandInvocationSource::ContextMenu
                                                            sender:sender];
}

- (void)OnRename:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileRenameCommandWithItems:m_Items
                                                               source:nc::core::CommandInvocationSource::ContextMenu
                                                               sender:sender];
}

- (void)OnPreview:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFilePreviewCommandWithItems:m_Items
                                                                 source:nc::core::CommandInvocationSource::ContextMenu
                                                                 sender:sender];
}

- (void)OnGetInfo:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileGetInfoCommandWithItems:m_Items
                                                                 source:nc::core::CommandInvocationSource::ContextMenu
                                                                 sender:sender];
}

- (void)OnCopyPathname:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileCopyPathCommandWithItems:m_Items
                                                                 source:nc::core::CommandInvocationSource::ContextMenu
                                                                 sender:sender];
}

- (void)OnCompressToOppositePanel:(id)sender
{
    m_CompressToOppositeAction->Perform(m_Panel, sender);
}

- (void)OnCompressToCurrentPanel:(id)sender
{
    [m_Panel.view.actionsDispatcher executeArchiveCreateCommandWithItems:m_Items
                                                                   source:nc::core::CommandInvocationSource::ContextMenu
                                                                   sender:sender];
}

- (void)OnExtractArchiveHere:(id)sender
{
    [m_Panel.view.actionsDispatcher executeArchiveExtractCommandWithItems:m_Items
                                                                   source:nc::core::CommandInvocationSource::ContextMenu
                                                                   sender:sender];
}

- (void)OnShareWithService:(id)sender
{
    auto delegate = [[NCPanelContextMenuSharingDelegate alloc] init];
    delegate.sourceWindow = m_Panel.window;

    NSSharingService *service = static_cast<NSMenuItem *>(sender).representedObject;
    service.delegate = delegate;
    [service performWithItems:m_ShareItemsURLs];
}

- (void)OnDuplicateItem:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileDuplicateCommandWithItems:m_Items
                                                                  source:nc::core::CommandInvocationSource::ContextMenu
                                                                  sender:sender];
}

- (void)OnCalculateSizes:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileCalculateSizesCommandWithItems:m_Items
                                                                        source:nc::core::CommandInvocationSource::ContextMenu
                                                                        sender:sender];
}

- (void)OnBatchRename:(id)sender
{
    [m_Panel.view.actionsDispatcher executeFileBatchRenameCommandWithItems:m_Items
                                                                     source:nc::core::CommandInvocationSource::ContextMenu
                                                                     sender:sender];
}

- (void)onTagItem:(id)_sender
{
    // TODO: somehow move this action code into actual actions
    NSMenuItem *it = nc::objc_cast<NSMenuItem>(_sender);
    if( !it )
        return;
    const auto tag = std::any_cast<nc::utility::Tags::Tag>(nc::objc_cast<AnyHolder>(it.representedObject).any);
    const auto state = it.state;
    dispatch_to_background([tag, state, items = m_Items] {
        pstld::for_each(items.begin(), items.end(), [&](const VFSListingItem &_item) {
            if( state == NSControlStateValueOn )
                nc::utility::Tags::RemoveTag(_item.Path(), tag.Label());
            else
                nc::utility::Tags::AddTag(_item.Path(), tag);
        });
    });
}

- (std::span<VFSListingItem>)items
{
    return m_Items;
}

@end

@implementation NCPanelContextMenuSharingDelegate {
    NCPanelContextMenuSharingDelegate *m_Self;
}
@synthesize sourceWindow;

- (instancetype)init
{
    self = [super init];
    if( self ) {
        m_Self = self;
    }
    return self;
}

- (void)sharingService:(NSSharingService *) [[maybe_unused]] sharingService
    didFailToShareItems:(NSArray *) [[maybe_unused]] items
                  error:(NSError *) [[maybe_unused]] error
{
    m_Self = nil;
}

- (void)sharingService:(NSSharingService *) [[maybe_unused]] sharingService
         didShareItems:(NSArray *) [[maybe_unused]] items
{
    m_Self = nil;
}

- (nullable NSWindow *)sharingService:(NSSharingService *) [[maybe_unused]] sharingService
            sourceWindowForShareItems:(NSArray *) [[maybe_unused]] items
                  sharingContentScope:(NSSharingContentScope *) [[maybe_unused]] sharingContentScope
{
    return self.sourceWindow;
}

@end
