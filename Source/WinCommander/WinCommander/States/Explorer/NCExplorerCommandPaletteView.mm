// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerCommandPaletteView.h"

#include <algorithm>

static const CGFloat g_PaletteWidth = 560.0;
static const CGFloat g_QueryHeight = 32.0;
static const CGFloat g_RowHeight = 28.0;
static const CGFloat g_Padding = 10.0;
/** Rows the list shows before it scrolls; also bounds how many results are ranked per keystroke. */
static const size_t g_VisibleRows = 10;

@interface NCExplorerCommandPaletteView () <NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation NCExplorerCommandPaletteView {
    NSTextField *m_Query;
    NSTableView *m_Table;
    NSScrollView *m_TableScroll;
    std::vector<nc::core::CommandPaletteEntry> m_Roster;
    std::vector<nc::core::CommandPaletteMatch> m_Matches;
    __weak id<NCExplorerCommandPaletteDelegate> _paletteDelegate;
}
@synthesize paletteDelegate = _paletteDelegate;

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( !self )
        return nil;
    self.material = NSVisualEffectMaterialHUDWindow;
    self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    self.state = NSVisualEffectStateActive;
    self.wantsLayer = true;
    self.layer.cornerRadius = 10.0;
    self.accessibilityElement = true;
    self.accessibilityRole = NSAccessibilityGroupRole;
    self.accessibilityIdentifier = @"wincommander.explorer.commandPalette";
    self.accessibilityLabel = NSLocalizedString(@"Command palette", "Command palette accessibility label");

    m_Query = [[NSTextField alloc] initWithFrame:NSZeroRect];
    m_Query.placeholderString = NSLocalizedString(@"Run a command…", "Command palette query placeholder");
    m_Query.bezeled = false;
    m_Query.drawsBackground = false;
    m_Query.focusRingType = NSFocusRingTypeNone;
    m_Query.font = [NSFont systemFontOfSize:18.0];
    m_Query.delegate = self;
    m_Query.translatesAutoresizingMaskIntoConstraints = false;
    m_Query.accessibilityIdentifier = @"wincommander.explorer.commandPalette.query";
    [self addSubview:m_Query];

    m_Table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    m_Table.headerView = nil;
    m_Table.rowHeight = g_RowHeight;
    m_Table.allowsEmptySelection = false;
    m_Table.allowsMultipleSelection = false;
    m_Table.backgroundColor = NSColor.clearColor;
    m_Table.dataSource = self;
    m_Table.delegate = self;
    m_Table.accessibilityIdentifier = @"wincommander.explorer.commandPalette.results";
    NSTableColumn *const column = [[NSTableColumn alloc] initWithIdentifier:@"command"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [m_Table addTableColumn:column];
    // A double click is the pointer equivalent of Return, so the palette is not keyboard-only.
    m_Table.target = self;
    m_Table.doubleAction = @selector(onTableDoubleClick:);

    m_TableScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    m_TableScroll.documentView = m_Table;
    m_TableScroll.hasVerticalScroller = true;
    m_TableScroll.drawsBackground = false;
    m_TableScroll.translatesAutoresizingMaskIntoConstraints = false;
    [self addSubview:m_TableScroll];

    [NSLayoutConstraint activateConstraints:@[
        [m_Query.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:g_Padding],
        [m_Query.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-g_Padding],
        [m_Query.topAnchor constraintEqualToAnchor:self.topAnchor constant:g_Padding],
        [m_Query.heightAnchor constraintEqualToConstant:g_QueryHeight],
        [m_TableScroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:g_Padding],
        [m_TableScroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-g_Padding],
        [m_TableScroll.topAnchor constraintEqualToAnchor:m_Query.bottomAnchor constant:g_Padding],
        [m_TableScroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-g_Padding],
    ]];
    return self;
}

+ (NSSize)preferredSize
{
    return NSMakeSize(g_PaletteWidth,
                      g_Padding * 3.0 + g_QueryHeight + (g_RowHeight * static_cast<CGFloat>(g_VisibleRows)));
}

- (void)setRoster:(std::vector<nc::core::CommandPaletteEntry>)_roster
{
    m_Roster = std::move(_roster);
    m_Query.stringValue = @"";
    [self refilter];
}

- (void)focusQueryField
{
    [self.window makeFirstResponder:m_Query];
}

- (void)refilter
{
    const std::string query = m_Query.stringValue.UTF8String ? m_Query.stringValue.UTF8String : "";
    m_Matches = nc::core::FilterCommandPalette(m_Roster, query, {.maximum_results = 200});
    [m_Table reloadData];
    // Ranking put the best candidate first, so the selection follows it rather than staying put -
    // otherwise the row under the cursor would drift away from the one the query now describes.
    if( !m_Matches.empty() ) {
        [m_Table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:false];
        [m_Table scrollRowToVisible:0];
    }
}

- (const nc::core::CommandPaletteEntry *)entryAtRow:(NSInteger)_row
{
    if( _row < 0 || static_cast<size_t>(_row) >= m_Matches.size() )
        return nullptr;
    const size_t index = m_Matches[static_cast<size_t>(_row)].entry_index;
    return index < m_Roster.size() ? &m_Roster[index] : nullptr;
}

- (BOOL)commitSelection
{
    const nc::core::CommandPaletteEntry *const entry = [self entryAtRow:m_Table.selectedRow];
    // A disabled row is shown so the command can be found, but committing it must do nothing -
    // the registry would refuse anyway, and silently closing the palette would look like it ran.
    if( !entry || !entry->enabled )
        return false;
    const std::string command_id = entry->id;
    id<NCExplorerCommandPaletteDelegate> const delegate = self.paletteDelegate;
    [delegate commandPaletteDidDismiss:self];
    [delegate commandPalette:self didChooseCommandId:command_id];
    return true;
}

- (BOOL)moveSelectionBy:(NSInteger)_delta
{
    if( m_Matches.empty() )
        return false;
    const NSInteger last = static_cast<NSInteger>(m_Matches.size()) - 1;
    const NSInteger current = m_Table.selectedRow < 0 ? 0 : m_Table.selectedRow;
    const NSInteger next = std::clamp<NSInteger>(current + _delta, 0, last);
    [m_Table selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(next)] byExtendingSelection:false];
    [m_Table scrollRowToVisible:next];
    return true;
}

- (void)onTableDoubleClick:(id) [[maybe_unused]] _sender
{
    [self commitSelection];
}

#pragma mark - NSTextFieldDelegate

- (void)controlTextDidChange:(NSNotification *) [[maybe_unused]] _notification
{
    [self refilter];
}

- (BOOL)control:(NSControl *) [[maybe_unused]] _control
               textView:(NSTextView *) [[maybe_unused]] _text_view
    doCommandBySelector:(SEL)_selector
{
    // Arrows and Return are the palette's own navigation while the query field holds focus; the
    // field would otherwise consume them as text editing and the list could never be driven.
    if( _selector == @selector(moveDown:) )
        return [self moveSelectionBy:1];
    if( _selector == @selector(moveUp:) )
        return [self moveSelectionBy:-1];
    if( _selector == @selector(insertNewline:) )
        return [self commitSelection];
    if( _selector == @selector(cancelOperation:) ) {
        [self.paletteDelegate commandPaletteDidDismiss:self];
        return true;
    }
    return false;
}

#pragma mark - NSTableView

- (NSInteger)numberOfRowsInTableView:(NSTableView *) [[maybe_unused]] _table
{
    return static_cast<NSInteger>(m_Matches.size());
}

- (NSView *)tableView:(NSTableView *) [[maybe_unused]] _table
    viewForTableColumn:(NSTableColumn *) [[maybe_unused]] _column
                    row:(NSInteger)_row
{
    const nc::core::CommandPaletteEntry *const entry = [self entryAtRow:_row];
    if( !entry )
        return nil;
    NSTextField *const label = [NSTextField labelWithString:@""];
    NSString *const title = [NSString stringWithUTF8String:entry->title.c_str()] ?: @"";
    NSString *const subtitle = [NSString stringWithUTF8String:entry->subtitle.c_str()] ?: @"";
    label.stringValue = subtitle.length > 0 ? [NSString stringWithFormat:@"%@  —  %@", title, subtitle] : title;
    label.textColor = entry->enabled ? NSColor.labelColor : NSColor.disabledControlTextColor;
    label.accessibilityLabel = title;
    return label;
}

#pragma mark - Testing

- (std::vector<std::string>)visibleCommandIdsForTesting
{
    std::vector<std::string> ids;
    ids.reserve(m_Matches.size());
    for( const nc::core::CommandPaletteMatch &match : m_Matches )
        if( match.entry_index < m_Roster.size() )
            ids.push_back(m_Roster[match.entry_index].id);
    return ids;
}

- (std::string)selectedCommandIdForTesting
{
    const nc::core::CommandPaletteEntry *const entry = [self entryAtRow:m_Table.selectedRow];
    return entry ? entry->id : std::string{};
}

- (void)setQueryForTesting:(NSString *)_query
{
    m_Query.stringValue = _query ?: @"";
    [self refilter];
}

- (BOOL)moveSelectionByForTesting:(NSInteger)_delta
{
    return [self moveSelectionBy:_delta];
}

- (BOOL)commitSelectionForTesting
{
    return [self commitSelection];
}

@end
