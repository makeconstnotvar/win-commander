// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerCommandBarView.h"
#include "NCExplorerPanePresentationModel.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../FilePanels/Helpers/Pasteboard.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <WinCommander/Core/Commands/OperationCancelCommand.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <WinCommander/States/CommandPresentationAdapter.h>
#include <CUI/CommandPopover.h>
#include <Operations/OperationCenterCoordinator.h>
#include <Panel/PanelData.h>
#include <Panel/PanelDataSortMode.h>
#include <Utility/ObjCpp.h>
#include <Utility/StringExtras.h>
#include <VFS/VFS.h>
#include <optional>

@interface NCExplorerCommandBarView () <NCCommandPopoverDelegate, NSSharingServicePickerDelegate>
@end

@interface NCExplorerOperationCancelMenuTarget : NSObject
- (instancetype)initWithContext:(nc::core::CommandContext)_context;
- (const nc::core::CommandContext &)context;
@end

@implementation NCExplorerOperationCancelMenuTarget {
    nc::core::CommandContext m_Context;
}

- (instancetype)initWithContext:(nc::core::CommandContext)_context
{
    self = [super init];
    if( self )
        m_Context = std::move(_context);
    return self;
}

- (const nc::core::CommandContext &)context
{
    return m_Context;
}

@end

namespace {

NSString *StringFromUTF8(const std::string_view _value)
{
    return [[NSString alloc] initWithBytes:_value.data() length:_value.size() encoding:NSUTF8StringEncoding];
}

NSString *UserFacingDisabledReason(const nc::core::DisabledReason &_reason)
{
    NSString *const key = StringFromUTF8(_reason.user_message_key);
    if( key.length ) {
        NSString *const localized = [NSBundle.mainBundle localizedStringForKey:key value:nil table:nil];
        if( localized.length && ![localized isEqualToString:key] )
            return localized;
    }
    return [NSBundle.mainBundle localizedStringForKey:@"commands.disabled.generic"
                                                 value:@"This command is currently unavailable"
                                                 table:nil];
}

NSString *OperationTypeTitle(const nc::ops::OperationPlanType _type)
{
    using enum nc::ops::OperationPlanType;
    switch( _type ) {
        case Copy:
            return NSLocalizedString(@"explorer.operations.type.copy", "Explorer operation menu type");
        case Move:
            return NSLocalizedString(@"explorer.operations.type.move", "Explorer operation menu type");
        case Rename:
            return NSLocalizedString(@"explorer.operations.type.rename", "Explorer operation menu type");
        case Trash:
            return NSLocalizedString(@"explorer.operations.type.trash", "Explorer operation menu type");
        case PermanentDelete:
            return NSLocalizedString(@"explorer.operations.type.delete", "Explorer operation menu type");
    }
    return NSLocalizedString(@"explorer.operations.type.operation", "Explorer operation menu type");
}

NSString *OperationStateTitle(const nc::ops::OperationRecordState _state)
{
    using enum nc::ops::OperationRecordState;
    switch( _state ) {
        case Queued:
            return NSLocalizedString(@"explorer.operations.state.queued", "Explorer operation menu state");
        case Running:
            return NSLocalizedString(@"explorer.operations.state.running", "Explorer operation menu state");
        case Paused:
            return NSLocalizedString(@"explorer.operations.state.paused", "Explorer operation menu state");
        case Cancelling:
            return NSLocalizedString(@"explorer.operations.state.cancelling", "Explorer operation menu state");
        case Finalizing:
            return NSLocalizedString(@"explorer.operations.state.finalizing", "Explorer operation menu state");
        case Interrupted:
            return NSLocalizedString(@"explorer.operations.state.interrupted", "Explorer operation menu state");
        case Cancelled:
            return NSLocalizedString(@"explorer.operations.state.cancelled", "Explorer operation menu state");
        case Failed:
            return NSLocalizedString(@"explorer.operations.state.failed", "Explorer operation menu state");
        case Completed:
            return NSLocalizedString(@"explorer.operations.state.completed", "Explorer operation menu state");
        case CompletedWithWarnings:
            return NSLocalizedString(@"explorer.operations.state.completedWithWarnings", "Explorer operation menu state");
    }
    return NSLocalizedString(@"explorer.operations.state.unknown", "Explorer operation menu state");
}

bool IsActiveOperationState(const nc::ops::OperationRecordState _state) noexcept
{
    using enum nc::ops::OperationRecordState;
    switch( _state ) {
        case Queued:
        case Running:
        case Paused:
        case Cancelling:
        case Finalizing:
            return true;
        case Interrupted:
        case Cancelled:
        case Failed:
        case Completed:
        case CompletedWithWarnings:
            return false;
    }
    return false;
}

} // namespace

@implementation NCExplorerCommandBarView {
    PanelController *m_Panel;
    NCCommandPopover *m_ActivePopover;
    NSSharingServicePicker *m_ActiveSharingPicker;
    NSButton *m_CutButton;
    NSButton *m_CopyButton;
    NSButton *m_PasteButton;
    NSButton *m_RenameButton;
    NSButton *m_ShareButton;
    NSButton *m_DeleteButton;
    NSTimer *m_PasteboardMonitor;
    NSInteger m_LastPasteboardChangeCount;
    std::optional<nc::explorer::PanePresentationModel> m_PanePresentation;
    std::weak_ptr<nc::ops::OperationCenterCoordinator> m_OperationCenter;
    nc::core::CommandRegistry *m_CommandRegistry;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    return [self initWithFrame:frameRect
                panelController:_panel
      operationCenterCoordinator:std::weak_ptr<nc::ops::OperationCenterCoordinator>{}
                 commandRegistry:nullptr];
}

- (instancetype)initWithFrame:(NSRect)frameRect
              panelController:(PanelController *)_panel
    operationCenterCoordinator:(std::weak_ptr<nc::ops::OperationCenterCoordinator>)_operation_center
               commandRegistry:(nc::core::CommandRegistry *)_command_registry
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        m_OperationCenter = std::move(_operation_center);
        m_CommandRegistry = _command_registry;
        m_PanePresentation.emplace(_panel.paneId);
        m_LastPasteboardChangeCount = NSPasteboard.generalPasteboard.changeCount;
        [self buildLayout];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(commandContextDidChange:)
                                                   name:NCPanelViewContextDidChangeNotification
                                                 object:m_Panel.view];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(commandContextDidChange:)
                                                   name:NSApplicationDidBecomeActiveNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(commandContextDidChange:)
                                                   name:nc::panel::NCPanelPasteboardCutStateDidChangeNotification
                                                 object:NSPasteboard.generalPasteboard];
        [self updateCommandAvailability];
    }
    return self;
}

- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
{
    dispatch_assert_queue(dispatch_get_main_queue());
    m_PanePresentation->Apply(_snapshot);
}

- (void)dealloc
{
    [m_PasteboardMonitor invalidate];
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [m_PasteboardMonitor invalidate];
    m_PasteboardMonitor = nil;
    if( self.window ) {
        __weak NCExplorerCommandBarView *weak_self = self;
        m_PasteboardMonitor = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                              repeats:true
                                                                block:^([[maybe_unused]] NSTimer *timer) {
          if( NCExplorerCommandBarView *const strong_self = weak_self )
              [strong_self checkPasteboardForChanges];
        }];
    }
    [self updateCommandAvailability];
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
    NSButton *const new_button = [self makeButtonWithTitle:NSLocalizedString(@"New", "Explorer command bar button")
                                                      symbol:@"plus"
                                                      target:self
                                                      action:@selector(showNewPopover:)];

    m_CutButton = [self makeButtonWithTitle:NSLocalizedString(@"Cut", "Explorer command bar button")
                                     symbol:@"scissors"
                                     target:self
                                     action:@selector(performCut:)];

    m_CopyButton = [self makeButtonWithTitle:NSLocalizedString(@"Copy", "Explorer command bar button")
                                      symbol:@"doc.on.doc"
                                      target:self
                                      action:@selector(performCopy:)];

    m_PasteButton =
        [self makeButtonWithTitle:NSLocalizedString(@"Paste", "Explorer command bar button")
                            symbol:@"doc.on.clipboard"
                            target:self
                            action:@selector(performPaste:)];

    m_RenameButton =
        [self makeButtonWithTitle:NSLocalizedString(@"Rename", "Explorer command bar button")
                            symbol:@"pencil"
                            target:self
                            action:@selector(performRename:)];

    m_ShareButton =
        [self makeButtonWithTitle:NSLocalizedString(@"Share", "Explorer command bar button")
                            symbol:@"square.and.arrow.up"
                            target:self
                            action:@selector(showSharePicker:)];

    // Explorer-style "Delete" is a move-to-trash, not a permanent delete - OnDeleteCommand:/
    // OnDeletePermanentlyCommand: are also available on the dispatcher but are deliberately not
    // used here.
    m_DeleteButton =
        [self makeButtonWithTitle:NSLocalizedString(@"Delete", "Explorer command bar button")
                            symbol:@"trash"
                            target:self
                            action:@selector(performDelete:)];

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
                                                       action:@selector(showMoreMenu:)];

    NSStackView *const stack = [NSStackView stackViewWithViews:@[
        new_button,
        m_CutButton,
        m_CopyButton,
        m_PasteButton,
        m_RenameButton,
        m_ShareButton,
        m_DeleteButton,
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

#pragma mark - Command validation

- (void)commandContextDidChange:(NSNotification *) [[maybe_unused]] _notification
{
    [self updateCommandAvailability];
}

- (void)checkPasteboardForChanges
{
    const NSInteger change_count = NSPasteboard.generalPasteboard.changeCount;
    if( change_count == m_LastPasteboardChangeCount )
        return;
    m_LastPasteboardChangeCount = change_count;
    [self updateCommandAvailability];
}

- (void)updateCommandAvailability
{
    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
    m_LastPasteboardChangeCount = NSPasteboard.generalPasteboard.changeCount;
    const auto cut_state =
        [dispatcher fileCutCommandStateFromSource:nc::core::CommandInvocationSource::Toolbar];
    nc::presentation::CommandPresentationAdapter::Apply(cut_state, m_CutButton);
    const auto copy_state =
        [dispatcher fileCopyCommandStateFromSource:nc::core::CommandInvocationSource::Toolbar];
    nc::presentation::CommandPresentationAdapter::Apply(copy_state, m_CopyButton);
    m_PasteButton.enabled = [dispatcher validateActionBySelector:@selector(paste:)];
    const auto rename_state =
        [dispatcher fileRenameCommandStateFromSource:nc::core::CommandInvocationSource::Toolbar];
    nc::presentation::CommandPresentationAdapter::Apply(rename_state, m_RenameButton);
    m_DeleteButton.enabled = [dispatcher validateActionBySelector:@selector(OnMoveToTrash:)];

    bool has_shareable_item = false;
    for( const VFSListingItem &item : m_Panel.selectedEntriesOrFocusedEntry ) {
        if( !item.IsDotDot() && item.Host() && item.Host()->IsNativeFS() ) {
            has_shareable_item = true;
            break;
        }
    }
    m_ShareButton.enabled = has_shareable_item;
}

- (void)performAction:(SEL)_selector sender:(id)_sender
{
    [m_Panel.view.actionsDispatcher executeBySelectorIfValidOrBeep:_selector withSender:_sender];
    [self updateCommandAvailability];
}

- (void)performCut:(id)_sender
{
    [m_Panel.view.actionsDispatcher executeFileCutCommandFromSource:nc::core::CommandInvocationSource::Toolbar
                                                             sender:_sender];
    [self updateCommandAvailability];
}

- (void)performCopy:(id)_sender
{
    [m_Panel.view.actionsDispatcher executeFileCopyCommandFromSource:nc::core::CommandInvocationSource::Toolbar
                                                              sender:_sender];
    [self updateCommandAvailability];
}

- (void)performPaste:(id)_sender
{
    [self performAction:@selector(paste:) sender:_sender];
}

- (void)performRename:(id)_sender
{
    [m_Panel.view.actionsDispatcher executeFileRenameCommandFromSource:nc::core::CommandInvocationSource::Toolbar
                                                                sender:_sender];
    [self updateCommandAvailability];
}

- (void)performDelete:(id)_sender
{
    [self performAction:@selector(OnMoveToTrash:) sender:_sender];
}

- (void)performPopoverAction:(id)_sender
{
    NCCommandPopoverItem *const item = nc::objc_cast<NCCommandPopoverItem>(_sender);
    if( !item || ![item.representedObject isKindOfClass:NSString.class] ) {
        NSBeep();
        return;
    }
    const SEL selector = NSSelectorFromString(static_cast<NSString *>(item.representedObject));
    if( selector == @selector(ToggleViewHiddenFiles:) ) {
        [m_Panel.view.actionsDispatcher
            executeViewToggleHiddenFilesCommandFromSource:nc::core::CommandInvocationSource::Toolbar
                                                  sender:item];
        [self updateCommandAvailability];
        return;
    }
    [self performAction:selector sender:item];
}

#pragma mark - New

- (void)showNewPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCCommandPopover *const popover =
        [[NCCommandPopover alloc] initWithTitle:NSLocalizedString(@"New", "Explorer command bar - New popover title")];

    NCCommandPopoverItem *const new_folder = [[NCCommandPopoverItem alloc] init];
    new_folder.title = NSLocalizedString(@"New Folder", "Explorer command bar - New popover item");
    new_folder.image = [NSImage imageWithSystemSymbolName:@"folder.badge.plus" accessibilityDescription:nil];
    new_folder.target = self;
    new_folder.action = @selector(performPopoverAction:);
    new_folder.representedObject = NSStringFromSelector(@selector(OnQuickNewFolder:));
    [popover addItem:new_folder];

    NCCommandPopoverItem *const new_file = [[NCCommandPopoverItem alloc] init];
    new_file.title = NSLocalizedString(@"New File", "Explorer command bar - New popover item");
    new_file.image = [NSImage imageWithSystemSymbolName:@"doc.badge.plus" accessibilityDescription:nil];
    new_file.target = self;
    new_file.action = @selector(performPopoverAction:);
    new_file.representedObject = NSStringFromSelector(@selector(OnQuickNewFile:));
    [popover addItem:new_file];

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - Sort

- (void)showSortPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCCommandPopover *const popover = [[NCCommandPopover alloc]
        initWithTitle:NSLocalizedString(@"Sort by", "Explorer command bar - Sort popover title")];

    // Mirrors the "Sort by ..." menu items in MainMenu.xib, all backed by the PanelAction structs
    // declared in Actions/ToggleSort.h (ToggleSortingByName, ToggleSortingByExtension, etc) and
    // registered on the dispatcher's action map under these exact selectors.
    NSArray<NSString *> *const sort_titles = @[
        NSLocalizedString(@"Name", "Explorer command bar - Sort popover item"),
        NSLocalizedString(@"Type", "Explorer command bar - Sort popover item"),
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
    static constexpr nc::core::PaneSortKey sort_keys[] = {
        nc::core::PaneSortKey::Name,
        nc::core::PaneSortKey::Extension,
        nc::core::PaneSortKey::Size,
        nc::core::PaneSortKey::ModifiedTime,
        nc::core::PaneSortKey::CreatedTime,
        nc::core::PaneSortKey::AddedTime,
        nc::core::PaneSortKey::AccessedTime,
    };

    for( NSUInteger i = 0; i < sort_titles.count; ++i ) {
        NCCommandPopoverItem *const item = [[NCCommandPopoverItem alloc] init];
        item.title = sort_titles[i];
        if( const auto direction = m_PanePresentation->ActiveSortDirection(sort_keys[i]) ) {
            const bool ascending = *direction == nc::core::PaneSortDirection::Ascending;
            item.image = [NSImage imageWithSystemSymbolName:ascending ? @"arrow.up" : @"arrow.down"
                                          accessibilityDescription:ascending
                                                                       ? NSLocalizedString(@"Ascending",
                                                                                           "Sort direction")
                                                                       : NSLocalizedString(@"Descending",
                                                                                           "Sort direction")];
        }
        item.target = self;
        item.action = @selector(performPopoverAction:);
        item.representedObject = NSStringFromSelector(sort_actions[i]);
        [popover addItem:item];
    }

    [popover addItem:NCCommandPopoverItem.separatorItem];
    [popover addItem:[NCCommandPopoverItem
                         sectionHeaderWithTitle:NSLocalizedString(@"Group by",
                                                                  "Explorer command bar - Group section")]];

    NCCommandPopoverItem *const no_grouping = [[NCCommandPopoverItem alloc] init];
    no_grouping.title = NSLocalizedString(@"None", "Explorer command bar - Group popover item");
    no_grouping.image = m_PanePresentation->NoGroupingMarkerActive()
                            ? [NSImage imageWithSystemSymbolName:@"checkmark" accessibilityDescription:nil]
                            : nil;
    no_grouping.target = self;
    no_grouping.action = @selector(disableGrouping:);
    [popover addItem:no_grouping];

    NSArray<NSString *> *const group_titles = @[
        NSLocalizedString(@"Name", "Explorer command bar - Group popover item"),
        NSLocalizedString(@"Type", "Explorer command bar - Group popover item"),
        NSLocalizedString(@"Size", "Explorer command bar - Group popover item"),
        NSLocalizedString(@"Date Modified", "Explorer command bar - Group popover item")
    ];
    static const SEL group_actions[] = {
        @selector(groupByName:), @selector(groupByType:), @selector(groupBySize:), @selector(groupByDateModified:)};
    static constexpr nc::core::PaneGroupingKey group_keys[] = {
        nc::core::PaneGroupingKey::Name,
        nc::core::PaneGroupingKey::Extension,
        nc::core::PaneGroupingKey::Size,
        nc::core::PaneGroupingKey::ModifiedTime,
    };

    for( NSUInteger i = 0; i < group_titles.count; ++i ) {
        NCCommandPopoverItem *const item = [[NCCommandPopoverItem alloc] init];
        item.title = group_titles[i];
        if( m_PanePresentation->GroupingMarkerActive(group_keys[i]) )
            item.image = [NSImage imageWithSystemSymbolName:@"checkmark" accessibilityDescription:nil];
        item.target = self;
        item.action = group_actions[i];
        [popover addItem:item];
    }

    [self presentPopover:popover relativeToButton:button];
}

- (void)enableGroupingForSortMode:(nc::panel::data::SortMode::Mode)_direct
                      reverseMode:(nc::panel::data::SortMode::Mode)_reverse
{
    auto sort_mode = m_Panel.data.SortMode();
    if( sort_mode.sort != _direct && sort_mode.sort != _reverse ) {
        sort_mode.sort = _direct;
        [m_Panel changeSortingModeTo:sort_mode];
    }
    m_Panel.view.explorerDetailsGroupingEnabled = true;
}

- (void)disableGrouping:(id) [[maybe_unused]] _sender
{
    m_Panel.view.explorerDetailsGroupingEnabled = false;
}

- (void)groupByName:(id) [[maybe_unused]] _sender
{
    using SortMode = nc::panel::data::SortMode;
    [self enableGroupingForSortMode:SortMode::SortByName reverseMode:SortMode::SortByNameRev];
}

- (void)groupByType:(id) [[maybe_unused]] _sender
{
    using SortMode = nc::panel::data::SortMode;
    [self enableGroupingForSortMode:SortMode::SortByExt reverseMode:SortMode::SortByExtRev];
}

- (void)groupBySize:(id) [[maybe_unused]] _sender
{
    using SortMode = nc::panel::data::SortMode;
    [self enableGroupingForSortMode:SortMode::SortBySize reverseMode:SortMode::SortBySizeRev];
}

- (void)groupByDateModified:(id) [[maybe_unused]] _sender
{
    using SortMode = nc::panel::data::SortMode;
    [self enableGroupingForSortMode:SortMode::SortByModTime reverseMode:SortMode::SortByModTimeRev];
}

#pragma mark - View

- (void)showViewPopover:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NCCommandPopover *const popover =
        [[NCCommandPopover alloc] initWithTitle:NSLocalizedString(@"View", "Explorer command bar - View popover title")];

    [popover addItem:[NCCommandPopoverItem
                          sectionHeaderWithTitle:NSLocalizedString(@"Layout", "Explorer command bar - View section")]];

    NSArray<NSString *> *const view_titles = @[
        NSLocalizedString(@"Small Icons", "Explorer command bar - View popover item"),
        NSLocalizedString(@"Details", "Explorer command bar - View popover item"),
        NSLocalizedString(@"Medium Icons", "Explorer command bar - View popover item"),
        NSLocalizedString(@"Large Icons", "Explorer command bar - View popover item"),
        NSLocalizedString(@"Content", "Explorer command bar - View popover item")
    ];
    NSArray<NSString *> *const view_symbols = @[
        @"square.grid.3x3",
        @"list.bullet",
        @"square.grid.2x2",
        @"square.grid.2x2.fill",
        @"rectangle.grid.1x2"
    ];
    static const SEL view_actions[] = {
        @selector(onToggleViewLayout1:),
        @selector(onToggleViewLayout2:),
        @selector(onToggleViewLayout3:),
        @selector(onToggleViewLayout4:),
        @selector(onToggleViewLayout5:)
    };

    for( NSUInteger i = 0; i < view_titles.count; ++i ) {
        NCCommandPopoverItem *const item = [[NCCommandPopoverItem alloc] init];
        item.title = view_titles[i];
        const bool active_layout = m_PanePresentation->LayoutMarkerActive(static_cast<int32_t>(i));
        NSString *const symbol = active_layout ? @"checkmark" : view_symbols[i];
        item.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
        item.target = self;
        item.action = @selector(performPopoverAction:);
        item.representedObject = NSStringFromSelector(view_actions[i]);
        [popover addItem:item];
    }

    [popover addItem:NCCommandPopoverItem.separatorItem];

    NCCommandPopoverItem *const show_hidden = [[NCCommandPopoverItem alloc] init];
    show_hidden.title =
        NSLocalizedString(@"commands.view.toggleHiddenFiles.title", "Show hidden files command title");
    show_hidden.representedObject = NSStringFromSelector(@selector(ToggleViewHiddenFiles:));
    const auto show_hidden_state = [m_Panel.view.actionsDispatcher
        viewToggleHiddenFilesCommandStateForVisibility:m_PanePresentation->HiddenFilesVisibility()
                                                source:nc::core::CommandInvocationSource::Toolbar];

    // NCCommandPopoverItem has no enabled, hidden, or check-state properties. Project the shared
    // command presentation through an AppKit menu-item proxy, then copy the capabilities the
    // popover model supports. A disabled item has no action and therefore remains fail closed.
    NSMenuItem *const presentation = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    const bool show_hidden_enabled =
        nc::presentation::CommandPresentationAdapter::Apply(show_hidden_state, presentation);
    show_hidden.toolTip = presentation.toolTip;
    switch( show_hidden_state.check_state ) {
        case nc::core::CommandCheckState::On:
            show_hidden.image = [NSImage imageWithSystemSymbolName:@"checkmark" accessibilityDescription:nil];
            break;
        case nc::core::CommandCheckState::Mixed:
            show_hidden.image = [NSImage imageWithSystemSymbolName:@"minus" accessibilityDescription:nil];
            break;
        case nc::core::CommandCheckState::Off:
            break;
    }
    if( show_hidden_enabled ) {
        show_hidden.target = self;
        show_hidden.action = @selector(performPopoverAction:);
    }
    if( !presentation.hidden )
        [popover addItem:show_hidden];

    [self presentPopover:popover relativeToButton:button];
}

#pragma mark - More

- (NSMenu *)buildMoreMenu
{
    NSMenu *const menu = [[NSMenu alloc] initWithTitle:NSLocalizedString(@"More", "Explorer command bar - More menu title")];

    const auto add_action = [&](NSString *const title, const SEL selector) {
        NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:title
                                                             action:@selector(performMoreMenuAction:)
                                                      keyEquivalent:@""];
        item.target = self;
        item.representedObject = NSStringFromSelector(selector);
        [menu addItem:item];
    };
    add_action(NSLocalizedString(@"Get Info", "Explorer command bar - More menu item"), @selector(OnFileAttributes:));
    add_action(NSLocalizedString(@"Compress", "Explorer command bar - More menu item"), @selector(onCompressItems:));
    add_action(NSLocalizedString(@"Copy Path", "Explorer command bar - More menu item"), @selector(OnCopyCurrentFilePath:));

    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *const operations_header =
        [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"explorer.operations.title", "Explorer operation menu")
                                    action:nil
                             keyEquivalent:@""];
    operations_header.enabled = false;
    [menu addItem:operations_header];

    const auto operation_center = m_OperationCenter.lock();
    if( !operation_center || !m_CommandRegistry ) {
        NSMenuItem *const unavailable =
            [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"explorer.operations.unavailable", "Explorer operation menu")
                                        action:nil
                                 keyEquivalent:@""];
        unavailable.enabled = false;
        unavailable.toolTip = NSLocalizedString(@"explorer.operations.unavailable.detail", "Explorer operation menu");
        unavailable.accessibilityHelp = unavailable.toolTip;
        [menu addItem:unavailable];
    }
    else {
        const auto records = operation_center->Model().Snapshot();
        bool has_active_record = false;
        for( const nc::ops::OperationRecord &record : records ) {
            if( IsActiveOperationState(record.state) ) {
                has_active_record = true;
                break;
            }
        }
        if( !has_active_record ) {
            NSMenuItem *const empty =
                [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"explorer.operations.noActive", "Explorer operation menu")
                                            action:nil
                                     keyEquivalent:@""];
            empty.enabled = false;
            [menu addItem:empty];
        }
        else {
            for( const nc::ops::OperationRecord &record : records ) {
                if( !IsActiveOperationState(record.state) )
                    continue;
                const std::string operation_id = record.operation_id.ToString();
                NSMenuItem *const record_item = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithFormat:@"%@ — %@ (%@)",
                                                              OperationTypeTitle(record.operation_type),
                                                              OperationStateTitle(record.state),
                                                              StringFromUTF8(operation_id)]
                         action:nil
                  keyEquivalent:@""];
                record_item.enabled = false;
                [menu addItem:record_item];

                const nc::core::CommandContext context = nc::core::OperationCancelContextFromRecord(
                    record, nc::core::CommandInvocationSource::Menu);
                NSMenuItem *const cancel = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithFormat:NSLocalizedString(@"explorer.operations.cancel", "Explorer operation menu"),
                                                              StringFromUTF8(operation_id)]
                         action:nil
                  keyEquivalent:@""];
                const auto state = m_CommandRegistry->QueryState(
                    nc::core::CommandId{nc::core::command_ids::OperationCancel}, context);
                const nc::core::CommandState presentation_state = nc::core::OperationCancelPresentationState(state);
                const bool enabled = nc::presentation::CommandPresentationAdapter::Apply(presentation_state, cancel);
                cancel.enabled = enabled;
                if( enabled ) {
                    cancel.target = self;
                    cancel.action = @selector(performOperationCancel:);
                    cancel.representedObject = [[NCExplorerOperationCancelMenuTarget alloc] initWithContext:context];
                }
                [menu addItem:cancel];
            }
        }
    }

    return menu;
}

- (void)showMoreMenu:(id)sender
{
    NSButton *const button = nc::objc_cast<NSButton>(sender);
    if( !button )
        return;

    NSMenu *const menu = [self buildMoreMenu];
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0.0, NSHeight(button.bounds)) inView:button];
}

- (void)performMoreMenuAction:(id)_sender
{
    NSMenuItem *const item = nc::objc_cast<NSMenuItem>(_sender);
    if( !item || ![item.representedObject isKindOfClass:NSString.class] ) {
        NSBeep();
        return;
    }
    [self performAction:NSSelectorFromString(static_cast<NSString *>(item.representedObject)) sender:item];
}

- (void)performOperationCancel:(id)_sender
{
    NSMenuItem *const item = nc::objc_cast<NSMenuItem>(_sender);
    NCExplorerOperationCancelMenuTarget *const target =
        item ? nc::objc_cast<NCExplorerOperationCancelMenuTarget>(item.representedObject) : nil;
    if( !target || !m_CommandRegistry ) {
        NSBeep();
        return;
    }

    const auto result = m_CommandRegistry->Execute(
        nc::core::CommandId{nc::core::command_ids::OperationCancel}, [target context]);
    if( result.status == nc::core::CommandRegistry::ExecutionStatus::Executed )
        return;

    NSAlert *const alert = [NSAlert new];
    alert.messageText = NSLocalizedString(@"explorer.operations.cancel.failureTitle", "Explorer operation cancel failure");
    if( result.disabled_reason ) {
        NSLog(@"Operation cancellation rejected: %@", StringFromUTF8(result.disabled_reason->technical_message));
        alert.informativeText = UserFacingDisabledReason(*result.disabled_reason);
    }
    else {
        alert.informativeText = NSLocalizedString(@"explorer.operations.cancel.failureFallback",
                                                  "Explorer operation cancel failure");
    }
    if( m_Panel.window )
        [alert beginSheetModalForWindow:m_Panel.window completionHandler:nil];
    else
        [alert runModal];
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
