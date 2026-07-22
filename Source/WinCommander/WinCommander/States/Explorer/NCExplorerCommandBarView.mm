// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerCommandBarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include <CUI/CommandPopover.h>
#include <Utility/ObjCpp.h>
#include <Utility/StringExtras.h>
#include <VFS/VFS.h>

@interface NCExplorerCommandBarView () <NCCommandPopoverDelegate, NSSharingServicePickerDelegate>
@end

@implementation NCExplorerCommandBarView {
    PanelController *m_Panel;
    NCCommandPopover *m_ActivePopover;
    NSSharingServicePicker *m_ActiveSharingPicker;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        [self buildLayout];
    }
    return self;
}

- (NSButton *)makeButtonWithTitle:(NSString *)_title symbol:(NSString *)_symbol target:(id)_target action:(SEL)_action
{
    NSButton *const button = [NSButton buttonWithTitle:_title
                                                  image:[NSImage imageWithSystemSymbolName:_symbol
                                                                  accessibilityDescription:nil]
                                                 target:_target
                                                 action:_action];
    button.imagePosition = NSImageLeft;
    button.bezelStyle = NSBezelStyleTexturedRounded;
    button.refusesFirstResponder = true;
    button.translatesAutoresizingMaskIntoConstraints = false;
    return button;
}

- (void)buildLayout
{
    // Same idiom as NCExplorerToolbarDelegate: fetch the panel's own action dispatcher and wire
    // buttons directly to its existing IBActions.
    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;

    NSButton *const new_button = [self makeButtonWithTitle:NSLocalizedString(@"New", "Explorer command bar button")
                                                      symbol:@"plus"
                                                      target:self
                                                      action:@selector(showNewPopover:)];

    // NOTE: the dispatcher has no dedicated "cut" selector - MainMenu.xib wires a standard "cut:"
    // edit action, but nothing in NCPanelControllerActionsDispatcher (or the wider codebase)
    // implements it. The app's actual clipboard model is Copy (writes the pasteboard) + Paste
    // (copies from the pasteboard) + moveItemHere: (moves from the pasteboard, the "paste as
    // move" landing action used at the destination). There is no source-side action that marks
    // items for a later destructive paste. Until such an action exists, Cut is wired to the same
    // copy: selector as Copy, so it is at least functional (puts the selection on the pasteboard)
    // rather than silently doing nothing.
    NSButton *const cut_button = [self makeButtonWithTitle:NSLocalizedString(@"Cut", "Explorer command bar button")
                                                      symbol:@"scissors"
                                                      target:dispatcher
                                                      action:@selector(copy:)];

    NSButton *const copy_button = [self makeButtonWithTitle:NSLocalizedString(@"Copy", "Explorer command bar button")
                                                       symbol:@"doc.on.doc"
                                                       target:dispatcher
                                                       action:@selector(copy:)];

    NSButton *const paste_button =
        [self makeButtonWithTitle:NSLocalizedString(@"Paste", "Explorer command bar button")
                            symbol:@"doc.on.clipboard"
                            target:dispatcher
                            action:@selector(paste:)];

    NSButton *const rename_button =
        [self makeButtonWithTitle:NSLocalizedString(@"Rename", "Explorer command bar button")
                            symbol:@"pencil"
                            target:dispatcher
                            action:@selector(OnRenameFileInPlace:)];

    NSButton *const share_button =
        [self makeButtonWithTitle:NSLocalizedString(@"Share", "Explorer command bar button")
                            symbol:@"square.and.arrow.up"
                            target:self
                            action:@selector(showSharePicker:)];

    // Explorer-style "Delete" is a move-to-trash, not a permanent delete - OnDeleteCommand:/
    // OnDeletePermanentlyCommand: are also available on the dispatcher but are deliberately not
    // used here.
    NSButton *const delete_button =
        [self makeButtonWithTitle:NSLocalizedString(@"Delete", "Explorer command bar button")
                            symbol:@"trash"
                            target:dispatcher
                            action:@selector(OnMoveToTrash:)];

    NSButton *const sort_button = [self makeButtonWithTitle:NSLocalizedString(@"Sort", "Explorer command bar button")
                                                       symbol:@"arrow.up.arrow.down"
                                                       target:self
                                                       action:@selector(showSortPopover:)];

    NSButton *const view_button = [self makeButtonWithTitle:NSLocalizedString(@"View", "Explorer command bar button")
                                                       symbol:@"square.grid.2x2"
                                                       target:self
                                                       action:@selector(showViewPopover:)];

    NSButton *const more_button = [self makeButtonWithTitle:NSLocalizedString(@"More", "Explorer command bar button")
                                                       symbol:@"ellipsis.circle"
                                                       target:self
                                                       action:@selector(showMorePopover:)];

    NSStackView *const stack = [NSStackView stackViewWithViews:@[
        new_button,
        cut_button,
        copy_button,
        paste_button,
        rename_button,
        share_button,
        delete_button,
        sort_button,
        view_button,
        more_button
    ]];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.alignment = NSLayoutAttributeCenterY;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing = 6.0;
    stack.translatesAutoresizingMaskIntoConstraints = false;
    [self addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8.0],
        [stack.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-8.0],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:self.topAnchor constant:4.0],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.bottomAnchor constant:-4.0]
    ]];
}

#pragma mark - New

- (void)showNewPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;

    NCCommandPopover *const popover =
        [[NCCommandPopover alloc] initWithTitle:NSLocalizedString(@"New", "Explorer command bar - New popover title")];

    NCCommandPopoverItem *const new_folder = [[NCCommandPopoverItem alloc] init];
    new_folder.title = NSLocalizedString(@"New Folder", "Explorer command bar - New popover item");
    new_folder.image = [NSImage imageWithSystemSymbolName:@"folder.badge.plus" accessibilityDescription:nil];
    new_folder.target = dispatcher;
    new_folder.action = @selector(OnQuickNewFolder:);
    [popover addItem:new_folder];

    NCCommandPopoverItem *const new_file = [[NCCommandPopoverItem alloc] init];
    new_file.title = NSLocalizedString(@"New File", "Explorer command bar - New popover item");
    new_file.image = [NSImage imageWithSystemSymbolName:@"doc.badge.plus" accessibilityDescription:nil];
    new_file.target = dispatcher;
    new_file.action = @selector(OnQuickNewFile:);
    [popover addItem:new_file];

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - Sort

- (void)showSortPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;

    NCCommandPopover *const popover = [[NCCommandPopover alloc]
        initWithTitle:NSLocalizedString(@"Sort by", "Explorer command bar - Sort popover title")];

    // Mirrors the "Sort by ..." menu items in MainMenu.xib, all backed by the PanelAction structs
    // declared in Actions/ToggleSort.h (ToggleSortingByName, ToggleSortingByExtension, etc) and
    // registered on the dispatcher's action map under these exact selectors.
    NSArray<NSString *> *const sort_titles = @[
        NSLocalizedString(@"Name", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Extension", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Size", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Date Modified", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Date Created", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Date Added", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Date Accessed", "Explorer command bar - Sort popover item")
    ];
    static const SEL sort_actions[] = {
        @selector(ToggleSortByName:),
        @selector(ToggleSortByExt:),
        @selector(ToggleSortBySize:),
        @selector(ToggleSortByMTime:),
        @selector(ToggleSortByBTime:),
        @selector(ToggleSortByAddTime:),
        @selector(ToggleSortByATime:)
    };

    for( NSUInteger i = 0; i < sort_titles.count; ++i ) {
        NCCommandPopoverItem *const item = [[NCCommandPopoverItem alloc] init];
        item.title = sort_titles[i];
        item.target = dispatcher;
        item.action = sort_actions[i];
        [popover addItem:item];
    }

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - View (placeholder)

- (void)showViewPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;

    NCCommandPopover *const popover =
        [[NCCommandPopover alloc] initWithTitle:NSLocalizedString(@"View", "Explorer command bar - View popover title")];

    // The one genuinely useful, already-existing toggle that belongs here.
    NCCommandPopoverItem *const show_hidden = [[NCCommandPopoverItem alloc] init];
    show_hidden.title = NSLocalizedString(@"Show Hidden Items", "Explorer command bar - View popover item");
    show_hidden.target = dispatcher;
    show_hidden.action = @selector(ToggleViewHiddenFiles:);
    [popover addItem:show_hidden];

    [popover addItem:NCCommandPopoverItem.separatorItem];
    [popover addItem:[NCCommandPopoverItem
                          sectionHeaderWithTitle:NSLocalizedString(
                                                      @"Layout and density options are coming soon",
                                                      "Explorer command bar - View popover placeholder")]];

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - More (placeholder)

- (void)showMorePopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;

    NCCommandPopover *const popover =
        [[NCCommandPopover alloc] initWithTitle:NSLocalizedString(@"More", "Explorer command bar - More popover title")];

    // A handful of already-existing dispatcher actions that don't have a dedicated button of
    // their own - a real overflow menu is out of scope for this pass.
    NCCommandPopoverItem *const get_info = [[NCCommandPopoverItem alloc] init];
    get_info.title = NSLocalizedString(@"Get Info", "Explorer command bar - More popover item");
    get_info.target = dispatcher;
    get_info.action = @selector(OnFileAttributes:);
    [popover addItem:get_info];

    NCCommandPopoverItem *const compress = [[NCCommandPopoverItem alloc] init];
    compress.title = NSLocalizedString(@"Compress", "Explorer command bar - More popover item");
    compress.target = dispatcher;
    compress.action = @selector(onCompressItems:);
    [popover addItem:compress];

    NCCommandPopoverItem *const copy_path = [[NCCommandPopoverItem alloc] init];
    copy_path.title = NSLocalizedString(@"Copy Path", "Explorer command bar - More popover item");
    copy_path.target = dispatcher;
    copy_path.action = @selector(OnCopyCurrentFilePath:);
    [popover addItem:copy_path];

    [popover addItem:NCCommandPopoverItem.separatorItem];
    [popover
        addItem:[NCCommandPopoverItem sectionHeaderWithTitle:NSLocalizedString(
                                                                   @"More commands are coming soon",
                                                                   "Explorer command bar - More popover placeholder")]];

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - Popover plumbing

- (void)presentPopover:(NCCommandPopover *)_popover relativeToButton:(NSButton *)_button
{
    _popover.delegate = self;
    m_ActivePopover = _popover;
    [_popover showRelativeToRect:_button.bounds ofView:_button alignment:NCCommandPopoverAlignment::Left];
}

- (void)commandPopoverDidClose:(NCCommandPopover *)_popover
{
    if( m_ActivePopover == _popover )
        m_ActivePopover = nil;
}

#pragma mark - Share

- (void)showSharePicker:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button || !m_Panel )
        return;

    NSMutableArray<NSURL *> *const urls = [NSMutableArray new];
    for( const VFSListingItem &item : m_Panel.selectedEntriesOrFocusedEntry ) {
        if( item.IsDotDot() )
            continue;
        if( !item.Host() || !item.Host()->IsNativeFS() )
            continue;
        if( NSString *const path = [NSString stringWithUTF8StdString:item.Path()] )
            if( NSURL *const url = [NSURL fileURLWithPath:path] )
                [urls addObject:url];
    }

    if( urls.count == 0 ) {
        // Nothing shareable is selected (e.g. non-native VFS items only) - bail out quietly.
        NSBeep();
        return;
    }

    NSSharingServicePicker *const picker = [[NSSharingServicePicker alloc] initWithItems:urls];
    picker.delegate = self;
    m_ActiveSharingPicker = picker;
    [picker showRelativeToRect:button.bounds ofView:button preferredEdge:NSMinYEdge];
}

- (void)sharingServicePicker:(NSSharingServicePicker *) [[maybe_unused]] _picker
        didChooseSharingService:(NSSharingService *) [[maybe_unused]] _service
{
    m_ActiveSharingPicker = nil;
}

@end
