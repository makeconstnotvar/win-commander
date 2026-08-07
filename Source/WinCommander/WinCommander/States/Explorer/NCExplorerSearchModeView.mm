// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerSearchModeView.h"

#include <Utility/StringExtras.h>
#include <algorithm>
#include <charconv>
#include <cmath>

using nc::core::SearchBackendDescriptor;
using nc::core::SearchBackendKind;
using nc::core::SearchBackendLimitation;
using nc::core::SearchBackendSupport;
using nc::core::SearchFileType;
using nc::core::SearchNameMatch;
using nc::core::SearchPhase;
using nc::core::SearchRequest;
using nc::core::SearchScope;
using nc::core::SearchSnapshot;

namespace {

constexpr CGFloat g_VisibleHeight = 156.0;

NSString *LocalizedValue(NSString *_key, NSString *_fallback)
{
    return [NSBundle.mainBundle localizedStringForKey:_key value:_fallback table:nil];
}

NSString *PhaseTitle(const SearchSnapshot &_snapshot)
{
    using enum SearchPhase;
    switch( _snapshot.phase ) {
        case Idle:
            return LocalizedValue(@"explorer.search.state.ready", @"Ready to search");
        case Preparing:
            return LocalizedValue(@"explorer.search.state.preparing", @"Preparing search…");
        case Running:
            return LocalizedValue(@"explorer.search.state.running", @"Searching…");
        case PartiallyCompleted:
            return LocalizedValue(@"explorer.search.state.partial", @"Partial results");
        case Completed:
            return LocalizedValue(@"explorer.search.state.completed", @"Search completed");
        case Cancelled:
            return LocalizedValue(@"explorer.search.state.cancelled", @"Search cancelled");
        case Failed:
            if( _snapshot.failure && !_snapshot.failure->detail.empty() )
                return [NSString stringWithFormat:LocalizedValue(@"explorer.search.state.failed.detail",
                                                                  @"Search failed: %@"),
                                                  [NSString stringWithUTF8StdString:_snapshot.failure->detail]];
            return LocalizedValue(@"explorer.search.state.failed", @"Search failed");
        case NoResults:
            return LocalizedValue(@"explorer.search.state.noResults", @"No results found");
        case TooManyResults:
            return LocalizedValue(@"explorer.search.state.tooManyResults", @"Too many results; refine the search");
        case IndexUnavailable:
            return LocalizedValue(@"explorer.search.state.indexUnavailable", @"Spotlight index is unavailable");
        case BackendUnavailable:
            return LocalizedValue(@"explorer.search.state.backendUnavailable",
                                  @"Search is unavailable for this location");
        case PermissionLimitedResults:
            return LocalizedValue(@"explorer.search.state.permissionLimited", @"Results are limited by permissions");
    }
    return @"";
}

NSString *BackendTitle(const SearchBackendDescriptor &_backend)
{
    NSString *const kind = _backend.kind == SearchBackendKind::Spotlight
                               ? LocalizedValue(@"explorer.search.backend.spotlight", @"Spotlight")
                               : LocalizedValue(@"explorer.search.backend.direct", @"Direct scan");
    NSString *support = @"";
    using enum SearchBackendSupport;
    switch( _backend.support ) {
        case Supported:
            break;
        case Unsupported:
            support = LocalizedValue(@"explorer.search.backend.unsupported", @"unsupported");
            break;
        case Unavailable:
            support = LocalizedValue(@"explorer.search.backend.unavailable", @"unavailable");
            break;
        case IndexUnavailable:
            support = LocalizedValue(@"explorer.search.backend.indexUnavailable", @"index unavailable");
            break;
    }
    return support.length == 0 ? kind : [NSString stringWithFormat:@"%@ — %@", kind, support];
}

NSString *LimitationTitle(const SearchBackendLimitation _limitation)
{
    using enum SearchBackendLimitation;
    switch( _limitation ) {
        case RecursiveScopeUnavailable:
            return LocalizedValue(@"explorer.search.limitation.recursive", @"Subfolder search is unavailable");
        case CurrentDiskScopeUnavailable:
            return LocalizedValue(@"explorer.search.limitation.disk", @"Disk-wide search is unavailable");
        case WholeMacScopeRequiresSpotlight:
            return LocalizedValue(@"explorer.search.limitation.wholeMac", @"Whole Mac search requires Spotlight");
        case ContentSearchUnavailable:
            return LocalizedValue(@"explorer.search.limitation.content", @"Content search is unavailable");
        case MetadataSearchUnavailable:
            return LocalizedValue(@"explorer.search.limitation.metadata", @"Metadata filters are unavailable");
        case HiddenItemsUnavailable:
            return LocalizedValue(@"explorer.search.limitation.hidden", @"Hidden items are excluded");
        case FullDiskAccessMissing:
            return LocalizedValue(@"explorer.search.limitation.fullDiskAccess",
                                  @"Full Disk Access is required for complete results");
        case PermissionDeniedLocations:
            return LocalizedValue(@"explorer.search.limitation.permissionDenied",
                                  @"Some locations could not be read");
        case ResultPathsUnavailable:
            return LocalizedValue(@"explorer.search.limitation.resultPaths",
                                  @"Some results changed before they could be displayed");
        case ProviderUnavailable:
            return LocalizedValue(@"explorer.search.limitation.provider", @"The current provider cannot search");
        case SpotlightUnavailable:
            return LocalizedValue(@"explorer.search.limitation.spotlight", @"Spotlight is unavailable");
        case SpotlightIndexUnavailable:
            return LocalizedValue(@"explorer.search.limitation.spotlightIndex", @"Spotlight indexing is unavailable");
    }
    return @"";
}

NSString *LimitationsTitle(const SearchSnapshot &_snapshot)
{
    if( !_snapshot.backend || _snapshot.backend->limitations.empty() )
        return @"";
    NSMutableArray<NSString *> *const titles = [NSMutableArray new];
    for( const SearchBackendLimitation limitation : _snapshot.backend->limitations ) {
        NSString *const title = LimitationTitle(limitation);
        if( title.length > 0 )
            [titles addObject:title];
    }
    return [titles componentsJoinedByString:@"; "];
}

bool IsRunning(const SearchPhase _phase) noexcept
{
    return _phase == SearchPhase::Preparing || _phase == SearchPhase::Running;
}

bool CanReveal(const SearchPhase _phase) noexcept
{
    return _phase == SearchPhase::Completed || _phase == SearchPhase::PartiallyCompleted ||
           _phase == SearchPhase::TooManyResults || _phase == SearchPhase::PermissionLimitedResults;
}

NSString *Trimmed(NSString *_value)
{
    return [_value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

std::string UTF8(NSString *_value)
{
    const char *const bytes = _value.UTF8String;
    return bytes ? bytes : "";
}

template <class T>
bool ReadOptionalNumber(NSTextField *_field, std::optional<T> &_value)
{
    NSString *const trimmed = Trimmed(_field.stringValue);
    if( trimmed.length == 0 ) {
        _value.reset();
        return true;
    }
    const std::string text = UTF8(trimmed);
    T parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if( error != std::errc{} || end != text.data() + text.size() )
        return false;
    _value = parsed;
    return true;
}

} // namespace

@implementation NCExplorerSearchModeView {
    NSSearchField *m_Query;
    NSPopUpButton *m_Scope;
    NSSegmentedControl *m_NameMatch;
    NSTextField *m_Extension;
    NSPopUpButton *m_FileType;
    NSTextField *m_MinimumSize;
    NSTextField *m_MaximumSize;
    NSTextField *m_ModifiedAfter;
    NSTextField *m_ModifiedBefore;
    NSTextField *m_ContentQuery;
    NSButton *m_IncludeHidden;
    NSButton *m_Start;
    NSButton *m_Cancel;
    NSButton *m_Reveal;
    NSButton *m_Close;
    NSTextField *m_Status;
    NSTextField *m_Backend;
    NSTextField *m_Location;
    NSTextField *m_Counts;
    NSTextField *m_Limitations;
    NSProgressIndicator *m_Progress;
    NSLayoutConstraint *m_Height;
    std::optional<SearchSnapshot> m_Snapshot;
    std::function<void(SearchRequest)> m_StartHandler;
    std::function<void()> m_CancelHandler;
    std::function<void()> m_RevealOriginalHandler;
    std::function<void()> m_CloseHandler;
}

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        self.translatesAutoresizingMaskIntoConstraints = false;
        self.hidden = true;
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityIdentifier = @"wincommander.explorer.searchMode";
        self.accessibilityLabel = LocalizedValue(@"explorer.search.accessibility.label", @"Search mode");
        [self buildLayout];
    }
    return self;
}

- (void)buildLayout
{
    const auto make_label = [](NSString *_identifier, NSColor *_color) {
        NSTextField *const label = [NSTextField labelWithString:@""];
        label.translatesAutoresizingMaskIntoConstraints = false;
        label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        label.textColor = _color;
        label.lineBreakMode = NSLineBreakByTruncatingMiddle;
        label.accessibilityIdentifier = _identifier;
        return label;
    };
    const auto make_button = ^NSButton *(NSString *_title, NSString *_identifier, SEL _action) {
        NSButton *const button = [NSButton buttonWithTitle:_title target:self action:_action];
        button.translatesAutoresizingMaskIntoConstraints = false;
        button.bezelStyle = NSBezelStyleRounded;
        button.controlSize = NSControlSizeSmall;
        button.accessibilityIdentifier = _identifier;
        button.accessibilityLabel = _title;
        return button;
    };
    const auto make_field = ^NSTextField *(NSString *_placeholder, NSString *_identifier, NSString *_label) {
        NSTextField *const field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        field.translatesAutoresizingMaskIntoConstraints = false;
        field.controlSize = NSControlSizeSmall;
        field.placeholderString = _placeholder;
        field.delegate = self;
        field.accessibilityIdentifier = _identifier;
        field.accessibilityLabel = _label;
        return field;
    };

    m_Query = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    m_Query.translatesAutoresizingMaskIntoConstraints = false;
    m_Query.placeholderString = LocalizedValue(@"explorer.search.query.placeholder", @"Name or pattern");
    m_Query.target = self;
    m_Query.action = @selector(startSearch:);
    m_Query.delegate = self;
    m_Query.accessibilityIdentifier = @"wincommander.explorer.searchMode.query";
    m_Query.accessibilityLabel = LocalizedValue(@"explorer.search.query.accessibility.label", @"Search query");
    m_Query.accessibilityHelp =
        LocalizedValue(@"explorer.search.query.accessibility.help", @"Press Return to start searching");

    m_Scope = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:false];
    m_Scope.translatesAutoresizingMaskIntoConstraints = false;
    m_Scope.controlSize = NSControlSizeSmall;
    m_Scope.accessibilityIdentifier = @"wincommander.explorer.searchMode.scope";
    m_Scope.accessibilityLabel = LocalizedValue(@"explorer.search.scope.accessibility.label", @"Search scope");
    m_Scope.target = self;
    m_Scope.action = @selector(filtersChanged:);
    const auto add_scope = [&](NSString *_title, SearchScope _scope) {
        [m_Scope addItemWithTitle:_title];
        m_Scope.lastItem.tag = static_cast<NSInteger>(_scope);
    };
    add_scope(LocalizedValue(@"explorer.search.scope.currentFolder", @"Current Folder"), SearchScope::CurrentFolder);
    add_scope(LocalizedValue(@"explorer.search.scope.subfolders", @"Subfolders"), SearchScope::Recursive);
    add_scope(LocalizedValue(@"explorer.search.scope.currentDisk", @"This Disk"), SearchScope::CurrentDisk);
    add_scope(LocalizedValue(@"explorer.search.scope.wholeMac", @"Spotlight / Whole Mac"),
              SearchScope::SpotlightWholeMac);

    m_NameMatch = [NSSegmentedControl
        segmentedControlWithLabels:@[LocalizedValue(@"explorer.search.match.contains", @"Contains"),
                                     LocalizedValue(@"explorer.search.match.exact", @"Exact")]
                   trackingMode:NSSegmentSwitchTrackingSelectOne
                         target:self
                         action:@selector(filtersChanged:)];
    m_NameMatch.translatesAutoresizingMaskIntoConstraints = false;
    m_NameMatch.controlSize = NSControlSizeSmall;
    m_NameMatch.selectedSegment = 0;
    m_NameMatch.accessibilityIdentifier = @"wincommander.explorer.searchMode.nameMatch";
    m_NameMatch.accessibilityLabel = LocalizedValue(@"explorer.search.match.accessibility.label", @"Name matching");

    m_Extension = make_field(LocalizedValue(@"explorer.search.extension.placeholder", @"Extension"),
                             @"wincommander.explorer.searchMode.extension",
                             LocalizedValue(@"explorer.search.extension.accessibility.label", @"File extension"));

    m_FileType = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:false];
    m_FileType.translatesAutoresizingMaskIntoConstraints = false;
    m_FileType.controlSize = NSControlSizeSmall;
    m_FileType.target = self;
    m_FileType.action = @selector(filtersChanged:);
    m_FileType.accessibilityIdentifier = @"wincommander.explorer.searchMode.fileType";
    m_FileType.accessibilityLabel = LocalizedValue(@"explorer.search.fileType.accessibility.label", @"File type");
    const auto add_file_type = [&](NSString *_title, SearchFileType _type) {
        [m_FileType addItemWithTitle:_title];
        m_FileType.lastItem.tag = static_cast<NSInteger>(_type);
    };
    add_file_type(LocalizedValue(@"explorer.search.fileType.any", @"Files & Folders"), SearchFileType::Any);
    add_file_type(LocalizedValue(@"explorer.search.fileType.files", @"Files"), SearchFileType::RegularFile);
    add_file_type(LocalizedValue(@"explorer.search.fileType.folders", @"Folders"), SearchFileType::Directory);

    m_MinimumSize = make_field(LocalizedValue(@"explorer.search.minimumSize.placeholder", @"Min bytes"),
                               @"wincommander.explorer.searchMode.minimumSize",
                               LocalizedValue(@"explorer.search.minimumSize.accessibility.label",
                                              @"Minimum size in bytes"));
    m_MaximumSize = make_field(LocalizedValue(@"explorer.search.maximumSize.placeholder", @"Max bytes"),
                               @"wincommander.explorer.searchMode.maximumSize",
                               LocalizedValue(@"explorer.search.maximumSize.accessibility.label",
                                              @"Maximum size in bytes"));
    m_ModifiedAfter = make_field(LocalizedValue(@"explorer.search.modifiedAfter.placeholder", @"Modified after (Unix)"),
                                 @"wincommander.explorer.searchMode.modifiedAfter",
                                 LocalizedValue(@"explorer.search.modifiedAfter.accessibility.label",
                                                @"Modified at or after Unix time"));
    m_ModifiedBefore =
        make_field(LocalizedValue(@"explorer.search.modifiedBefore.placeholder", @"Modified before (Unix)"),
                   @"wincommander.explorer.searchMode.modifiedBefore",
                   LocalizedValue(@"explorer.search.modifiedBefore.accessibility.label",
                                  @"Modified at or before Unix time"));
    m_ContentQuery = make_field(LocalizedValue(@"explorer.search.content.placeholder", @"File content"),
                                @"wincommander.explorer.searchMode.content",
                                LocalizedValue(@"explorer.search.content.accessibility.label", @"File content query"));
    m_IncludeHidden = [NSButton
        checkboxWithTitle:LocalizedValue(@"explorer.search.includeHidden", @"Include Hidden")
                    target:self
                    action:@selector(filtersChanged:)];
    m_IncludeHidden.translatesAutoresizingMaskIntoConstraints = false;
    m_IncludeHidden.controlSize = NSControlSizeSmall;
    m_IncludeHidden.accessibilityIdentifier = @"wincommander.explorer.searchMode.includeHidden";
    m_IncludeHidden.accessibilityLabel =
        LocalizedValue(@"explorer.search.includeHidden.accessibility.label", @"Include hidden items");

    m_Start = make_button(LocalizedValue(@"explorer.search.start", @"Start"),
                          @"wincommander.explorer.searchMode.start",
                          @selector(startSearch:));
    m_Cancel = make_button(LocalizedValue(@"explorer.search.cancel", @"Cancel"),
                           @"wincommander.explorer.searchMode.cancel",
                           @selector(cancelSearch:));
    m_Reveal = make_button(LocalizedValue(@"explorer.search.reveal", @"Reveal Original"),
                           @"wincommander.explorer.searchMode.revealOriginal",
                           @selector(revealOriginal:));
    m_Close = make_button(LocalizedValue(@"explorer.search.close", @"Close"),
                          @"wincommander.explorer.searchMode.close",
                          @selector(closeSearch:));

    m_Status = make_label(@"wincommander.explorer.searchMode.status", NSColor.labelColor);
    m_Status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightMedium];
    m_Status.accessibilityLabel = LocalizedValue(@"explorer.search.status.accessibility.label", @"Search status");
    m_Backend = make_label(@"wincommander.explorer.searchMode.backend", NSColor.secondaryLabelColor);
    m_Backend.accessibilityLabel = LocalizedValue(@"explorer.search.backend.accessibility.label", @"Search backend");
    m_Location = make_label(@"wincommander.explorer.searchMode.location", NSColor.secondaryLabelColor);
    m_Location.accessibilityLabel =
        LocalizedValue(@"explorer.search.location.accessibility.label", @"Current location");
    m_Counts = make_label(@"wincommander.explorer.searchMode.counts", NSColor.secondaryLabelColor);
    m_Counts.accessibilityLabel = LocalizedValue(@"explorer.search.counts.accessibility.label", @"Search counts");
    m_Limitations = make_label(@"wincommander.explorer.searchMode.limitations", NSColor.systemOrangeColor);
    m_Limitations.accessibilityLabel =
        LocalizedValue(@"explorer.search.limitations.accessibility.label", @"Search limitations");

    m_Progress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    m_Progress.translatesAutoresizingMaskIntoConstraints = false;
    m_Progress.style = NSProgressIndicatorStyleBar;
    m_Progress.minValue = 0.0;
    m_Progress.maxValue = 1.0;
    m_Progress.displayedWhenStopped = true;
    m_Progress.accessibilityIdentifier = @"wincommander.explorer.searchMode.progress";
    m_Progress.accessibilityLabel = LocalizedValue(@"explorer.search.progress.accessibility.label", @"Search progress");

    NSStackView *const controls =
        [NSStackView stackViewWithViews:@[m_Query, m_Scope, m_NameMatch, m_Start, m_Cancel, m_Reveal, m_Close]];
    controls.translatesAutoresizingMaskIntoConstraints = false;
    controls.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    controls.alignment = NSLayoutAttributeCenterY;
    controls.spacing = 6.0;

    NSStackView *const metadata_filters = [NSStackView stackViewWithViews:@[
        m_Extension, m_FileType, m_MinimumSize, m_MaximumSize, m_ModifiedAfter, m_ModifiedBefore
    ]];
    metadata_filters.translatesAutoresizingMaskIntoConstraints = false;
    metadata_filters.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    metadata_filters.alignment = NSLayoutAttributeCenterY;
    metadata_filters.spacing = 6.0;

    NSStackView *const content_filters = [NSStackView stackViewWithViews:@[m_ContentQuery, m_IncludeHidden]];
    content_filters.translatesAutoresizingMaskIntoConstraints = false;
    content_filters.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    content_filters.alignment = NSLayoutAttributeCenterY;
    content_filters.spacing = 8.0;

    NSStackView *const details = [NSStackView stackViewWithViews:@[m_Status, m_Backend, m_Location, m_Counts]];
    details.translatesAutoresizingMaskIntoConstraints = false;
    details.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    details.alignment = NSLayoutAttributeCenterY;
    details.spacing = 10.0;

    NSStackView *const content = [NSStackView
        stackViewWithViews:@[controls, metadata_filters, content_filters, details, m_Limitations, m_Progress]];
    content.translatesAutoresizingMaskIntoConstraints = false;
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 5.0;
    [self addSubview:content];

    m_Height = [self.heightAnchor constraintEqualToConstant:0.0];
    m_Height.active = true;
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:10.0],
        [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-10.0],
        [content.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_Query.widthAnchor constraintGreaterThanOrEqualToConstant:160.0],
        [m_Query.widthAnchor constraintEqualToAnchor:controls.widthAnchor multiplier:0.28],
        [m_Extension.widthAnchor constraintEqualToConstant:86.0],
        [m_FileType.widthAnchor constraintEqualToConstant:120.0],
        [m_MinimumSize.widthAnchor constraintEqualToConstant:95.0],
        [m_MaximumSize.widthAnchor constraintEqualToConstant:95.0],
        [m_ModifiedAfter.widthAnchor constraintEqualToConstant:150.0],
        [m_ModifiedBefore.widthAnchor constraintEqualToConstant:150.0],
        [m_ContentQuery.widthAnchor constraintEqualToConstant:300.0],
        [m_Progress.widthAnchor constraintEqualToAnchor:content.widthAnchor],
        [m_Progress.heightAnchor constraintEqualToConstant:6.0],
    ]];
}

- (void)drawRect:(NSRect)_dirty_rect
{
    [NSColor.controlBackgroundColor setFill];
    NSRectFill(_dirty_rect);
    [NSColor.separatorColor setFill];
    NSRectFill(NSMakeRect(NSMinX(self.bounds), NSMinY(self.bounds), NSWidth(self.bounds), 1.0));
}

- (void)applySnapshot:(const std::optional<SearchSnapshot> &)_snapshot
    resultSelectionEligible:(bool)_result_selection_eligible
{
    dispatch_assert_queue(dispatch_get_main_queue());
    m_Snapshot = _snapshot;
    if( !_snapshot ) {
        [m_Progress stopAnimation:nil];
        self.hidden = true;
        m_Height.constant = 0.0;
        self.accessibilityValue = @"";
        return;
    }

    const SearchSnapshot &snapshot = *_snapshot;
    self.hidden = false;
    m_Height.constant = g_VisibleHeight;
    if( snapshot.request ) {
        m_Query.stringValue = [NSString stringWithUTF8StdString:snapshot.request->query];
        [m_Scope selectItemWithTag:static_cast<NSInteger>(snapshot.request->scope)];
        m_NameMatch.selectedSegment = snapshot.request->filters.name_match == SearchNameMatch::Exact ? 1 : 0;
        m_Extension.stringValue = snapshot.request->filters.extension
                                      ? [NSString stringWithUTF8StdString:*snapshot.request->filters.extension]
                                      : @"";
        [m_FileType selectItemWithTag:static_cast<NSInteger>(snapshot.request->filters.file_type)];
        m_MinimumSize.stringValue = snapshot.request->filters.size.minimum_bytes
                                        ? [NSString stringWithFormat:@"%llu",
                                                                     static_cast<unsigned long long>(
                                                                         *snapshot.request->filters.size.minimum_bytes)]
                                        : @"";
        m_MaximumSize.stringValue = snapshot.request->filters.size.maximum_bytes
                                        ? [NSString stringWithFormat:@"%llu",
                                                                     static_cast<unsigned long long>(
                                                                         *snapshot.request->filters.size.maximum_bytes)]
                                        : @"";
        m_ModifiedAfter.stringValue = snapshot.request->filters.modified.earliest_seconds
                                          ? [NSString stringWithFormat:@"%lld",
                                                                       static_cast<long long>(
                                                                           *snapshot.request->filters.modified
                                                                                .earliest_seconds)]
                                          : @"";
        m_ModifiedBefore.stringValue = snapshot.request->filters.modified.latest_seconds
                                           ? [NSString stringWithFormat:@"%lld",
                                                                        static_cast<long long>(
                                                                            *snapshot.request->filters.modified
                                                                                 .latest_seconds)]
                                           : @"";
        m_ContentQuery.stringValue = snapshot.request->filters.content
                                         ? [NSString stringWithUTF8StdString:*snapshot.request->filters.content]
                                         : @"";
        m_IncludeHidden.state = snapshot.request->filters.include_hidden ? NSControlStateValueOn
                                                                         : NSControlStateValueOff;
    }

    m_Status.stringValue = PhaseTitle(snapshot);
    m_Backend.stringValue = snapshot.backend ? BackendTitle(*snapshot.backend) : @"";
    m_Location.stringValue =
        snapshot.current_location
            ? [NSString stringWithFormat:LocalizedValue(@"explorer.search.location", @"Location: %@"),
                                         [NSString stringWithUTF8StdString:*snapshot.current_location]]
            : @"";
    m_Location.toolTip = snapshot.current_location ? [NSString stringWithUTF8StdString:*snapshot.current_location]
                                                   : @"";
    if( snapshot.scanned_count || snapshot.found_count ) {
        const unsigned long long scanned = snapshot.scanned_count.value_or(0);
        const unsigned long long found = snapshot.found_count.value_or(0);
        m_Counts.stringValue = [NSString
            stringWithFormat:LocalizedValue(@"explorer.search.counts", @"Scanned: %llu · Found: %llu"), scanned, found];
    }
    else
        m_Counts.stringValue = @"";
    m_Limitations.stringValue = LimitationsTitle(snapshot);

    m_Backend.hidden = m_Backend.stringValue.length == 0;
    m_Location.hidden = m_Location.stringValue.length == 0;
    m_Counts.hidden = m_Counts.stringValue.length == 0;
    m_Limitations.hidden = m_Limitations.stringValue.length == 0;

    const bool running = IsRunning(snapshot.phase);
    const bool valid_progress = snapshot.determinate_progress && std::isfinite(*snapshot.determinate_progress) &&
                                *snapshot.determinate_progress >= 0.0 && *snapshot.determinate_progress <= 1.0;
    m_Progress.indeterminate = !valid_progress;
    if( valid_progress ) {
        [m_Progress stopAnimation:nil];
        m_Progress.doubleValue = std::clamp(*snapshot.determinate_progress, 0.0, 1.0);
        m_Progress.accessibilityValue = @(*snapshot.determinate_progress);
    }
    else {
        m_Progress.doubleValue = 0.0;
        m_Progress.accessibilityValue = LocalizedValue(@"explorer.search.progress.indeterminate", @"Indeterminate");
        if( running )
            [m_Progress startAnimation:nil];
        else
            [m_Progress stopAnimation:nil];
    }

    m_Query.enabled = !running;
    m_Scope.enabled = !running;
    m_NameMatch.enabled = !running;
    m_Extension.enabled = !running;
    m_FileType.enabled = !running;
    m_MinimumSize.enabled = !running;
    m_MaximumSize.enabled = !running;
    m_ModifiedAfter.enabled = !running;
    m_ModifiedBefore.enabled = !running;
    m_ContentQuery.enabled = !running;
    m_IncludeHidden.enabled = !running;
    [self updateStartEnabled];
    m_Cancel.enabled = running;
    m_Reveal.enabled = _result_selection_eligible && CanReveal(snapshot.phase);

    NSArray<NSString *> *const accessibility_parts = @[
        m_Status.stringValue,
        m_Backend.stringValue,
        m_Location.stringValue,
        m_Counts.stringValue,
        m_Limitations.stringValue,
    ];
    NSMutableArray<NSString *> *const nonempty = [NSMutableArray new];
    for( NSString *const part : accessibility_parts )
        if( part.length > 0 )
            [nonempty addObject:part];
    self.accessibilityValue = [nonempty componentsJoinedByString:@", "];
    self.accessibilityHelp = m_Limitations.stringValue;
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

- (void)setStartHandler:(std::function<void(SearchRequest)>)_handler
{
    m_StartHandler = std::move(_handler);
}

- (BOOL)focusQueryField
{
    dispatch_assert_queue(dispatch_get_main_queue());
    return !self.hidden && self.window && [self.window makeFirstResponder:m_Query];
}

- (void)setCancelHandler:(std::function<void()>)_handler
{
    m_CancelHandler = std::move(_handler);
}

- (void)setRevealOriginalHandler:(std::function<void()>)_handler
{
    m_RevealOriginalHandler = std::move(_handler);
}

- (void)setCloseHandler:(std::function<void()>)_handler
{
    m_CloseHandler = std::move(_handler);
}

- (void)startSearch:(id)_sender
{
    (void)_sender;
    if( !m_Start.enabled || !m_StartHandler )
        return;
    SearchRequest request;
    if( ![self readRequest:request] )
        return;
    m_StartHandler(std::move(request));
}

- (void)controlTextDidChange:(NSNotification *)_notification
{
    (void)_notification;
    [self updateStartEnabled];
}

- (void)filtersChanged:(id)_sender
{
    (void)_sender;
    [self updateStartEnabled];
}

- (BOOL)readRequest:(SearchRequest &)_request
{
    _request = {};
    _request.query = UTF8(Trimmed(m_Query.stringValue));
    _request.scope = static_cast<SearchScope>(m_Scope.selectedItem.tag);
    _request.filters.name_match = m_NameMatch.selectedSegment == 1 ? SearchNameMatch::Exact
                                                                   : SearchNameMatch::Contains;

    NSString *const extension = Trimmed(m_Extension.stringValue);
    if( extension.length > 0 ) {
        const std::string value = UTF8(extension);
        const size_t first_character = value.find_first_not_of('.');
        if( first_character == std::string::npos || value.find('/') != std::string::npos )
            return false;
        _request.filters.extension = value;
    }
    _request.filters.file_type = static_cast<SearchFileType>(m_FileType.selectedItem.tag);
    if( !ReadOptionalNumber(m_MinimumSize, _request.filters.size.minimum_bytes) ||
        !ReadOptionalNumber(m_MaximumSize, _request.filters.size.maximum_bytes) ||
        !ReadOptionalNumber(m_ModifiedAfter, _request.filters.modified.earliest_seconds) ||
        !ReadOptionalNumber(m_ModifiedBefore, _request.filters.modified.latest_seconds) )
        return false;
    if( _request.filters.size.minimum_bytes && _request.filters.size.maximum_bytes &&
        *_request.filters.size.minimum_bytes > *_request.filters.size.maximum_bytes )
        return false;
    if( _request.filters.modified.earliest_seconds && _request.filters.modified.latest_seconds &&
        *_request.filters.modified.earliest_seconds > *_request.filters.modified.latest_seconds )
        return false;

    NSString *const content = Trimmed(m_ContentQuery.stringValue);
    if( content.length > 0 )
        _request.filters.content = UTF8(content);
    _request.filters.include_hidden = m_IncludeHidden.state == NSControlStateValueOn;

    return !_request.query.empty() || _request.filters.extension ||
           _request.filters.file_type != SearchFileType::Any || _request.filters.size.minimum_bytes ||
           _request.filters.size.maximum_bytes || _request.filters.modified.earliest_seconds ||
           _request.filters.modified.latest_seconds || _request.filters.content;
}

- (void)updateStartEnabled
{
    const bool running = m_Snapshot && IsRunning(m_Snapshot->phase);
    SearchRequest ignored;
    m_Start.enabled = !running && [self readRequest:ignored];
}

- (void)cancelSearch:(id)_sender
{
    (void)_sender;
    if( m_Cancel.enabled && m_CancelHandler )
        m_CancelHandler();
}

- (void)revealOriginal:(id)_sender
{
    (void)_sender;
    if( m_Reveal.enabled && m_RevealOriginalHandler )
        m_RevealOriginalHandler();
}

- (void)closeSearch:(id)_sender
{
    (void)_sender;
    if( m_CloseHandler )
        m_CloseHandler();
}

- (BOOL)performKeyEquivalent:(NSEvent *)_event
{
    if( _event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask )
        return [super performKeyEquivalent:_event];
    NSString *const characters = _event.charactersIgnoringModifiers;
    if( [characters isEqualToString:@"\r"] ) {
        const BOOL was_enabled = m_Start.enabled;
        if( was_enabled )
            [self startSearch:nil];
        return was_enabled;
    }
    if( [characters isEqualToString:@"\e"] ) {
        if( m_Cancel.enabled )
            [self cancelSearch:nil];
        else
            [self closeSearch:nil];
        return true;
    }
    return [super performKeyEquivalent:_event];
}

// Private observability and actions for focused AppKit tests.
- (CGFloat)visibleHeightForTesting { return m_Height.constant; }
- (NSSearchField *)queryForTesting { return m_Query; }
- (NSPopUpButton *)scopeForTesting { return m_Scope; }
- (NSSegmentedControl *)nameMatchForTesting { return m_NameMatch; }
- (NSTextField *)extensionForTesting { return m_Extension; }
- (NSPopUpButton *)fileTypeForTesting { return m_FileType; }
- (NSTextField *)minimumSizeForTesting { return m_MinimumSize; }
- (NSTextField *)maximumSizeForTesting { return m_MaximumSize; }
- (NSTextField *)modifiedAfterForTesting { return m_ModifiedAfter; }
- (NSTextField *)modifiedBeforeForTesting { return m_ModifiedBefore; }
- (NSTextField *)contentQueryForTesting { return m_ContentQuery; }
- (NSButton *)includeHiddenForTesting { return m_IncludeHidden; }
- (NSButton *)startForTesting { return m_Start; }
- (NSButton *)cancelForTesting { return m_Cancel; }
- (NSButton *)revealForTesting { return m_Reveal; }
- (NSButton *)closeForTesting { return m_Close; }
- (NSString *)statusForTesting { return m_Status.stringValue; }
- (NSString *)backendForTesting { return m_Backend.stringValue; }
- (NSString *)locationForTesting { return m_Location.stringValue; }
- (NSString *)countsForTesting { return m_Counts.stringValue; }
- (NSString *)limitationsForTesting { return m_Limitations.stringValue; }
- (NSProgressIndicator *)progressForTesting { return m_Progress; }

@end
