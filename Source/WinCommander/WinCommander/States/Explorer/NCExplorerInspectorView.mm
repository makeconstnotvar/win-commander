// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerInspectorView.h"

#include <WinCommander/States/FilePanels/Gallery/PanelGalleryCentralView.h>
#include <WinCommander/States/FilePanels/Views/QuickLookVFSBridge.h>
#include <Utility/UTI.h>
#include <array>
#include <memory>
#include <sys/stat.h>

namespace {

NSString *ToNSString(const std::string &_value)
{
    NSString *const string = [[NSString alloc] initWithBytes:_value.data()
                                                      length:_value.size()
                                                    encoding:NSUTF8StringEncoding];
    return string ?: @"";
}

NSString *FormatSize(const std::optional<uint64_t> &_size)
{
    if( !_size )
        return @"—";
    NSString *const adaptive = [NSByteCountFormatter stringFromByteCount:static_cast<long long>(*_size)
                                                              countStyle:NSByteCountFormatterCountStyleFile];
    return [NSString stringWithFormat:@"%@ (%llu bytes)", adaptive, static_cast<unsigned long long>(*_size)];
}

NSString *FormatDate(const std::optional<time_t> &_value, NSDateFormatter *_formatter)
{
    if( !_value )
        return @"—";
    return [_formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(*_value)]]
               ?: @"—";
}

NSString *FormatPermissions(const mode_t _mode)
{
    std::array<char, 10> text = {'-', '-', '-', '-', '-', '-', '-', '-', '-', '\0'};
    text[0] = (_mode & S_IRUSR) ? 'r' : '-';
    text[1] = (_mode & S_IWUSR) ? 'w' : '-';
    text[2] = (_mode & S_IXUSR) ? 'x' : '-';
    text[3] = (_mode & S_IRGRP) ? 'r' : '-';
    text[4] = (_mode & S_IWGRP) ? 'w' : '-';
    text[5] = (_mode & S_IXGRP) ? 'x' : '-';
    text[6] = (_mode & S_IROTH) ? 'r' : '-';
    text[7] = (_mode & S_IWOTH) ? 'w' : '-';
    text[8] = (_mode & S_IXOTH) ? 'x' : '-';
    if( _mode & S_ISUID )
        text[2] = (_mode & S_IXUSR) ? 's' : 'S';
    if( _mode & S_ISGID )
        text[5] = (_mode & S_IXGRP) ? 's' : 'S';
    if( _mode & S_ISVTX )
        text[8] = (_mode & S_IXOTH) ? 't' : 'T';
    return [NSString stringWithFormat:@"%s (%04o)", text.data(), static_cast<unsigned>(_mode & 07777)];
}

NSTextField *ValueLabel(NSString *_identifier, NSString *_accessibility_label)
{
    NSTextField *const label = [NSTextField labelWithString:@""];
    label.translatesAutoresizingMaskIntoConstraints = false;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.selectable = true;
    label.accessibilityIdentifier = _identifier;
    label.accessibilityLabel = _accessibility_label;
    return label;
}

NSStackView *MetadataRow(NSString *_title, NSTextField *_value)
{
    NSTextField *const title = [NSTextField labelWithString:_title];
    title.translatesAutoresizingMaskIntoConstraints = false;
    title.textColor = NSColor.secondaryLabelColor;
    title.alignment = NSTextAlignmentRight;
    [title.widthAnchor constraintEqualToConstant:82.0].active = true;

    NSStackView *const row = [NSStackView stackViewWithViews:@[title, _value]];
    row.translatesAutoresizingMaskIntoConstraints = false;
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeFirstBaseline;
    row.spacing = 10.0;
    [_value setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    return row;
}

} // namespace

@implementation NCExplorerInspectorView {
    std::unique_ptr<nc::explorer::InspectorModel> m_Model;
    NCPanelGalleryCentralView *m_PreviewView;
    NSStackView *m_RootStack;
    NSStackView *m_RefreshRow;
    NSProgressIndicator *m_RefreshIndicator;
    NSTextField *m_RefreshLabel;
    NSTextField *m_ErrorBanner;
    NSTextField *m_StatusLabel;
    NSTextField *m_MultipleSummary;
    NSTextField *m_FilenameLabel;
    NSStackView *m_MetadataStack;
    NSTextField *m_PathValue;
    NSTextField *m_SizeValue;
    NSTextField *m_CreatedValue;
    NSTextField *m_ModifiedValue;
    NSTextField *m_AccessedValue;
    NSTextField *m_TagsValue;
    NSTextField *m_PermissionsValue;
    NSTextField *m_OwnerGroupValue;
    NSDateFormatter *m_DateFormatter;
}

- (instancetype)initWithFrame:(NSRect)_frame
                       paneID:(nc::core::PaneId)_pane_id
                        UTIDB:(const nc::utility::UTIDB &)_UTIDB
    QLHazardousExtensionsList:(const std::string &)_ql_hazard_list
                  QLVFSBridge:(nc::panel::QuickLookVFSBridge &)_ql_vfs_bridge
{
    self = [super initWithFrame:_frame];
    if( !self )
        return nil;

    m_Model = std::make_unique<nc::explorer::InspectorModel>(_pane_id);
    self.translatesAutoresizingMaskIntoConstraints = false;
    self.accessibilityIdentifier = @"wincommander.explorer.inspector";
    self.accessibilityLabel = NSLocalizedString(@"File details and preview", "Explorer inspector accessibility label");

    m_DateFormatter = [NSDateFormatter new];
    m_DateFormatter.dateStyle = NSDateFormatterMediumStyle;
    m_DateFormatter.timeStyle = NSDateFormatterShortStyle;

    m_ErrorBanner = [NSTextField labelWithString:@""];
    m_ErrorBanner.translatesAutoresizingMaskIntoConstraints = false;
    m_ErrorBanner.textColor = NSColor.systemRedColor;
    m_ErrorBanner.lineBreakMode = NSLineBreakByWordWrapping;
    m_ErrorBanner.maximumNumberOfLines = 3;
    m_ErrorBanner.accessibilityIdentifier = @"wincommander.explorer.inspector.error";
    m_ErrorBanner.accessibilityLabel = NSLocalizedString(@"Inspector error", "Explorer accessibility label");

    m_RefreshIndicator = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    m_RefreshIndicator.translatesAutoresizingMaskIntoConstraints = false;
    m_RefreshIndicator.style = NSProgressIndicatorStyleSpinning;
    m_RefreshIndicator.controlSize = NSControlSizeSmall;
    m_RefreshIndicator.indeterminate = true;
    m_RefreshIndicator.accessibilityIdentifier = @"wincommander.explorer.inspector.refreshing";
    m_RefreshIndicator.accessibilityLabel =
        NSLocalizedString(@"Refreshing file details", "Explorer inspector accessibility label");
    m_RefreshLabel =
        [NSTextField labelWithString:NSLocalizedString(@"Refreshing…", "Explorer inspector refresh indicator")];
    m_RefreshLabel.textColor = NSColor.secondaryLabelColor;
    m_RefreshRow = [NSStackView stackViewWithViews:@[m_RefreshIndicator, m_RefreshLabel]];
    m_RefreshRow.translatesAutoresizingMaskIntoConstraints = false;
    m_RefreshRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    m_RefreshRow.spacing = 6.0;

    m_StatusLabel = [NSTextField labelWithString:@""];
    m_StatusLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_StatusLabel.alignment = NSTextAlignmentCenter;
    m_StatusLabel.textColor = NSColor.secondaryLabelColor;
    m_StatusLabel.lineBreakMode = NSLineBreakByWordWrapping;
    m_StatusLabel.maximumNumberOfLines = 3;
    m_StatusLabel.accessibilityIdentifier = @"wincommander.explorer.inspector.status";
    m_StatusLabel.accessibilityLabel = NSLocalizedString(@"Inspector status", "Explorer accessibility label");

    m_PreviewView = [[NCPanelGalleryCentralView alloc] initWithFrame:NSMakeRect(0, 0, _frame.size.width, 180)
                                                               UTIDB:_UTIDB
                                           QLHazardousExtensionsList:_ql_hazard_list
                                                         QLVFSBridge:_ql_vfs_bridge];
    m_PreviewView.accessibilityIdentifier = @"wincommander.explorer.inspector.preview";
    m_PreviewView.accessibilityLabel = NSLocalizedString(@"File preview", "Explorer accessibility label");
    [m_PreviewView.heightAnchor constraintEqualToConstant:180.0].active = true;

    m_FilenameLabel = [NSTextField labelWithString:@""];
    m_FilenameLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_FilenameLabel.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold];
    m_FilenameLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    m_FilenameLabel.selectable = true;
    m_FilenameLabel.accessibilityIdentifier = @"wincommander.explorer.inspector.filename";
    m_FilenameLabel.accessibilityLabel = NSLocalizedString(@"File name", "Explorer accessibility label");

    m_PathValue = ValueLabel(@"wincommander.explorer.inspector.path", NSLocalizedString(@"Path", "Metadata label"));
    m_SizeValue = ValueLabel(@"wincommander.explorer.inspector.size", NSLocalizedString(@"Size", "Metadata label"));
    m_CreatedValue =
        ValueLabel(@"wincommander.explorer.inspector.created", NSLocalizedString(@"Created", "Metadata label"));
    m_ModifiedValue =
        ValueLabel(@"wincommander.explorer.inspector.modified", NSLocalizedString(@"Modified", "Metadata label"));
    m_AccessedValue =
        ValueLabel(@"wincommander.explorer.inspector.accessed", NSLocalizedString(@"Accessed", "Metadata label"));
    m_TagsValue = ValueLabel(@"wincommander.explorer.inspector.tags", NSLocalizedString(@"Tags", "Metadata label"));
    m_PermissionsValue = ValueLabel(@"wincommander.explorer.inspector.permissions",
                                    NSLocalizedString(@"POSIX permissions", "Metadata label"));
    m_OwnerGroupValue = ValueLabel(@"wincommander.explorer.inspector.ownerGroup",
                                   NSLocalizedString(@"Owner and group", "Metadata label"));

    m_MetadataStack = [NSStackView stackViewWithViews:@[
        MetadataRow(NSLocalizedString(@"Path", "Metadata label"), m_PathValue),
        MetadataRow(NSLocalizedString(@"Size", "Metadata label"), m_SizeValue),
        MetadataRow(NSLocalizedString(@"Created", "Metadata label"), m_CreatedValue),
        MetadataRow(NSLocalizedString(@"Modified", "Metadata label"), m_ModifiedValue),
        MetadataRow(NSLocalizedString(@"Accessed", "Metadata label"), m_AccessedValue),
        MetadataRow(NSLocalizedString(@"Tags", "Metadata label"), m_TagsValue),
        MetadataRow(NSLocalizedString(@"Permissions", "Metadata label"), m_PermissionsValue),
        MetadataRow(NSLocalizedString(@"Owner / Group", "Metadata label"), m_OwnerGroupValue),
    ]];
    m_MetadataStack.translatesAutoresizingMaskIntoConstraints = false;
    m_MetadataStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    m_MetadataStack.alignment = NSLayoutAttributeLeading;
    m_MetadataStack.spacing = 7.0;

    m_MultipleSummary = [NSTextField labelWithString:@""];
    m_MultipleSummary.translatesAutoresizingMaskIntoConstraints = false;
    m_MultipleSummary.alignment = NSTextAlignmentCenter;
    m_MultipleSummary.lineBreakMode = NSLineBreakByWordWrapping;
    m_MultipleSummary.maximumNumberOfLines = 2;
    m_MultipleSummary.accessibilityIdentifier = @"wincommander.explorer.inspector.multipleSummary";
    m_MultipleSummary.accessibilityLabel = NSLocalizedString(@"Selection summary", "Explorer accessibility label");

    m_RootStack = [NSStackView stackViewWithViews:@[
        m_ErrorBanner,
        m_RefreshRow,
        m_StatusLabel,
        m_PreviewView,
        m_FilenameLabel,
        m_MetadataStack,
        m_MultipleSummary,
    ]];
    m_RootStack.translatesAutoresizingMaskIntoConstraints = false;
    m_RootStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    m_RootStack.alignment = NSLayoutAttributeLeading;
    m_RootStack.spacing = 10.0;
    [self addSubview:m_RootStack];

    [NSLayoutConstraint activateConstraints:@[
        [m_RootStack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:12.0],
        [m_RootStack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12.0],
        [m_RootStack.topAnchor constraintEqualToAnchor:self.topAnchor constant:12.0],
        [m_RootStack.bottomAnchor constraintLessThanOrEqualToAnchor:self.bottomAnchor constant:-12.0],
        [m_PreviewView.widthAnchor constraintEqualToAnchor:m_RootStack.widthAnchor],
        [m_MetadataStack.widthAnchor constraintEqualToAnchor:m_RootStack.widthAnchor],
        [m_StatusLabel.widthAnchor constraintEqualToAnchor:m_RootStack.widthAnchor],
        [m_MultipleSummary.widthAnchor constraintEqualToAnchor:m_RootStack.widthAnchor],
    ]];

    [self renderModel];
    return self;
}

- (BOOL)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
{
    if( !m_Model->Apply(_snapshot) )
        return false;
    [self renderModel];
    return true;
}

- (BOOL)rebindToPaneID:(const nc::core::PaneId)_pane_id
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( _pane_id.value == 0 )
        return false;
    [m_PreviewView clearPreview];
    m_Model = std::make_unique<nc::explorer::InspectorModel>(_pane_id);
    [self renderModel];
    return true;
}

- (void)clearPreview
{
    [m_PreviewView clearPreview];
}

- (void)renderModel
{
    const auto state = m_Model->State();
    const bool single = state == nc::explorer::InspectorState::Single;
    const bool multiple = state == nc::explorer::InspectorState::Multiple;

    m_ErrorBanner.hidden = !m_Model->Error();
    if( m_Model->Error() ) {
        const std::string &message = m_Model->Error()->user_message.empty() ? m_Model->Error()->technical_message
                                                                            : m_Model->Error()->user_message;
        m_ErrorBanner.stringValue = ToNSString(message);
        m_ErrorBanner.accessibilityValue = m_ErrorBanner.stringValue;
        m_ErrorBanner.accessibilityHelp = m_ErrorBanner.stringValue;
    }
    else {
        m_ErrorBanner.stringValue = @"";
        m_ErrorBanner.accessibilityValue = nil;
        m_ErrorBanner.accessibilityHelp = nil;
    }

    m_RefreshRow.hidden = !m_Model->IsRefreshing();
    if( m_Model->IsRefreshing() )
        [m_RefreshIndicator startAnimation:nil];
    else
        [m_RefreshIndicator stopAnimation:nil];

    m_StatusLabel.hidden = single || multiple;
    m_PreviewView.hidden = !single;
    m_FilenameLabel.hidden = !single;
    m_MetadataStack.hidden = !single;
    m_MultipleSummary.hidden = !multiple;

    if( !single )
        [m_PreviewView clearPreview];

    switch( state ) {
        case nc::explorer::InspectorState::Hidden:
            m_StatusLabel.stringValue =
                NSLocalizedString(@"Select an item to see details", "Explorer inspector initial state");
            break;
        case nc::explorer::InspectorState::Empty:
            m_StatusLabel.stringValue = NSLocalizedString(@"No item selected", "Explorer inspector empty state");
            break;
        case nc::explorer::InspectorState::PaneLoading:
            m_StatusLabel.stringValue = NSLocalizedString(@"Loading folder…", "Explorer inspector loading state");
            break;
        case nc::explorer::InspectorState::PaneError:
            m_StatusLabel.stringValue = NSLocalizedString(@"Unable to load folder", "Explorer inspector failed state");
            break;
        case nc::explorer::InspectorState::Single:
        case nc::explorer::InspectorState::Multiple:
            m_StatusLabel.stringValue = @"";
            break;
    }
    m_StatusLabel.accessibilityValue = m_StatusLabel.stringValue;

    if( single ) {
        const nc::core::FileMetadataSnapshot &metadata = m_Model->Items().front();
        m_FilenameLabel.stringValue = ToNSString(metadata.filename);
        m_FilenameLabel.accessibilityValue = m_FilenameLabel.stringValue;
        m_PathValue.stringValue = ToNSString(metadata.path);
        m_SizeValue.stringValue = FormatSize(metadata.size);
        m_CreatedValue.stringValue = FormatDate(metadata.created_time, m_DateFormatter);
        m_ModifiedValue.stringValue = FormatDate(metadata.modified_time, m_DateFormatter);
        m_AccessedValue.stringValue = FormatDate(metadata.accessed_time, m_DateFormatter);

        NSMutableArray<NSString *> *const tags = [NSMutableArray arrayWithCapacity:metadata.tags.size()];
        for( const nc::core::FileMetadataTag &tag : metadata.tags )
            [tags addObject:ToNSString(tag.label)];
        m_TagsValue.stringValue = tags.count == 0 ? @"—" : [tags componentsJoinedByString:@", "];
        m_PermissionsValue.stringValue = FormatPermissions(metadata.unix_mode);
        NSString *const uid = metadata.unix_uid ? [NSString stringWithFormat:@"%u", *metadata.unix_uid] : @"—";
        NSString *const gid = metadata.unix_gid ? [NSString stringWithFormat:@"%u", *metadata.unix_gid] : @"—";
        m_OwnerGroupValue.stringValue = [NSString stringWithFormat:@"%@ / %@", uid, gid];

        for( NSTextField *const value in @[
                 m_PathValue,
                 m_SizeValue,
                 m_CreatedValue,
                 m_ModifiedValue,
                 m_AccessedValue,
                 m_TagsValue,
                 m_PermissionsValue,
                 m_OwnerGroupValue,
             ] )
            value.accessibilityValue = value.stringValue;

        [m_PreviewView showVFSItem:m_Model->PreviewItem()];
    }
    else if( multiple ) {
        uint64_t total_size = 0;
        bool has_complete_size = true;
        for( const nc::core::FileMetadataSnapshot &metadata : m_Model->Items() ) {
            if( metadata.size )
                total_size += *metadata.size;
            else
                has_complete_size = false;
        }
        NSString *const count =
            [NSString stringWithFormat:NSLocalizedString(@"%lu items selected", "Explorer inspector multi-selection"),
                                       static_cast<unsigned long>(m_Model->Items().size())];
        m_MultipleSummary.stringValue =
            has_complete_size ? [NSString stringWithFormat:@"%@ · %@", count, FormatSize(total_size)] : count;
        m_MultipleSummary.accessibilityValue = m_MultipleSummary.stringValue;
    }
}

- (nc::explorer::InspectorState)renderedStateForTesting
{
    return m_Model->State();
}

- (BOOL)previewVisibleForTesting
{
    return !m_PreviewView.hidden;
}

- (BOOL)refreshingVisibleForTesting
{
    return !m_RefreshRow.hidden;
}

- (BOOL)errorVisibleForTesting
{
    return !m_ErrorBanner.hidden;
}

- (NSString *)statusTextForTesting
{
    return m_StatusLabel.stringValue;
}

- (NSString *)errorTextForTesting
{
    return m_ErrorBanner.stringValue;
}

- (NSString *)multipleSummaryForTesting
{
    return m_MultipleSummary.stringValue;
}

- (NSTextField *)filenameFieldForTesting
{
    return m_FilenameLabel;
}

- (NSTextField *)pathFieldForTesting
{
    return m_PathValue;
}

- (NSTextField *)sizeFieldForTesting
{
    return m_SizeValue;
}

- (NSTextField *)createdFieldForTesting
{
    return m_CreatedValue;
}

- (NSTextField *)modifiedFieldForTesting
{
    return m_ModifiedValue;
}

- (NSTextField *)accessedFieldForTesting
{
    return m_AccessedValue;
}

- (NSTextField *)tagsFieldForTesting
{
    return m_TagsValue;
}

- (NSTextField *)permissionsFieldForTesting
{
    return m_PermissionsValue;
}

- (NSTextField *)ownerGroupFieldForTesting
{
    return m_OwnerGroupValue;
}

- (NCPanelGalleryCentralView *)previewViewForTesting
{
    return m_PreviewView;
}

@end
