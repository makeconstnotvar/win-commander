// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerBreadcrumbControl.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../FilePanels/PanelView.h"
#include "../../Core/Errors/FileManagerErrorAdapter.h"
#include "../../Core/Pane/PaneSnapshot.h"
#include "../../Core/VisualState/VisualStateMapper.h"
#include <Base/CommonPaths.h>
#include <Base/dispatch_cpp.h>
#include <Utility/PathManip.h>
#include <VFS/VFS.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

using namespace std::chrono_literals;

using namespace nc;

namespace {

constexpr CGFloat g_InnerControlHeight = 28.0;
constexpr CGFloat g_SearchFieldWidth = 230.0;

struct BreadcrumbPresentationState {
    core::PaneLoadPhase load_phase = core::PaneLoadPhase::Empty;
    bool address_editable = false;
    bool is_uniform = false;
    std::string path;
    std::string display_title;
    VFSHostPtr host;

    bool operator==(const BreadcrumbPresentationState &) const noexcept = default;
};

bool IsAddressEditable(const BreadcrumbPresentationState &_state)
{
    return _state.address_editable;
}

bool HasAddressLocation(const BreadcrumbPresentationState &_state)
{
    return _state.is_uniform && _state.host != nullptr && _state.host != VFSHost::DummyHost() && !_state.path.empty();
}

bool HasSameAddressContext(const BreadcrumbPresentationState &_lhs, const BreadcrumbPresentationState &_rhs)
{
    return _lhs.is_uniform == _rhs.is_uniform && _lhs.path == _rhs.path && _lhs.host == _rhs.host;
}

bool HasSameBreadcrumbContent(const BreadcrumbPresentationState &_lhs, const BreadcrumbPresentationState &_rhs)
{
    return HasSameAddressContext(_lhs, _rhs) && _lhs.display_title == _rhs.display_title;
}

NSString *StringFromUTF8(const std::string &_value)
{
    return [[NSString alloc] initWithBytes:_value.data() length:_value.size() encoding:NSUTF8StringEncoding];
}

NSString *LocalizedVisualMessage(const core::VisualMessage &_message)
{
    NSString *const fallback = StringFromUTF8(_message.user_message_fallback) ?: @"";
    NSString *const key = StringFromUTF8(_message.user_message_key);
    if( key.length == 0 )
        return fallback.length == 0 ? nil : fallback;

    NSString *const value = [NSBundle.mainBundle localizedStringForKey:key value:fallback table:nil];
    if( value.length == 0 || ([value isEqualToString:key] && fallback.length != 0) )
        return fallback.length == 0 ? nil : fallback;
    return value;
}

NSString *_Nullable NavigationRequestErrorText(const Error &_error,
                                               const panel::DirectoryChangeResultSource _source,
                                               const core::FileManagerErrorContext &_context)
{
    if( _source == panel::DirectoryChangeResultSource::Admission && _error.Domain() == Error::POSIX ) {
        switch( _error.Code() ) {
            case EBUSY:
                return NSLocalizedString(@"Another folder request is already in progress.",
                                         "Explorer navigation admission error");
            case ENODEV:
                return NSLocalizedString(@"Folder navigation is unavailable.", "Explorer navigation admission error");
            case EINVAL:
                return NSLocalizedString(@"The folder path is invalid.", "Explorer navigation admission error");
        }
    }

    const core::FileManagerError mapped = core::FileManagerErrorAdapter::FromError(_error, _context);
    if( mapped.category == core::FileManagerErrorCategory::OperationCancelledError ||
        mapped.severity == core::FileManagerErrorSeverity::Info )
        return nil;
    const core::VisualMessage message{
        .user_message_key = mapped.user_message_key,
        .user_message_fallback = mapped.user_message,
        .suggested_actions = mapped.suggested_actions,
    };
    return LocalizedVisualMessage(message) ?: @"The operation could not be completed.";
}

std::string ExpandAddressPath(std::string_view _input, const BreadcrumbPresentationState &_state)
{
    const std::string_view home =
        _state.host->IsNativeFS() ? std::string_view{base::CommonPaths::Home()} : std::string_view{"/"};
    return utility::PathManip::Expand(_input, home, _state.path).string();
}

} // namespace

@interface NCExplorerBreadcrumbTarget : NSObject

- (instancetype)initWithTitle:(NSString *)_title path:(std::string)_path host:(VFSHostPtr)_host;
@property(nonatomic, readonly) NSString *title;
@property(nonatomic, readonly) const std::string &path;
@property(nonatomic, readonly) const VFSHostPtr &host;

@end

@implementation NCExplorerBreadcrumbTarget {
    NSString *m_Title;
    std::string m_Path;
    VFSHostPtr m_Host;
}

- (instancetype)initWithTitle:(NSString *)_title path:(std::string)_path host:(VFSHostPtr)_host
{
    self = [super init];
    if( self ) {
        m_Title = [_title copy];
        m_Path = std::move(_path);
        m_Host = std::move(_host);
    }
    return self;
}

- (NSString *)title
{
    return m_Title;
}

- (const std::string &)path
{
    return m_Path;
}

- (const VFSHostPtr &)host
{
    return m_Host;
}

@end

@interface NCExplorerBreadcrumbControl () <NSComboBoxDataSource,
                                           NSComboBoxDelegate,
                                           NSTextFieldDelegate,
                                           NSMenuItemValidation>
@end

@implementation NCExplorerBreadcrumbControl {
    PanelController *m_Panel;
    NSButton *m_LocationButton;
    NSStackView *m_PathStack;
    NSComboBox *m_PathEditor;
    NSTextField *m_FallbackLabel;
    NSTextField *m_ErrorLabel;
    NSSearchField *m_FindField;
    NSProgressIndicator *m_BusyIndicator;
    NSMutableArray<NCExplorerBreadcrumbTarget *> *m_SegmentTargets;
    NSArray<NSString *> *m_Completions;
    std::shared_ptr<std::atomic_bool> m_CompletionCancellation;
    unsigned long m_PathGeneration;
    unsigned long m_CompletionGeneration;
    unsigned long m_NavigationGeneration;
    NSString *m_RequestErrorMessage;
    std::optional<unsigned long> m_LocationGeneration;
    std::optional<BreadcrumbPresentationState> m_PresentationState;
}

@synthesize panelController = m_Panel;

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityLabel = NSLocalizedString(@"Address bar", "Explorer accessibility label");
        [self buildLayout];
    }
    return self;
}

- (void)rebindToPanelController:(PanelController *)_panel
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !_panel || m_Panel == _panel )
        return;

    ++m_NavigationGeneration;
    ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    m_CompletionCancellation.reset();
    m_Completions = @[];
    [m_PathEditor reloadData];
    m_PathEditor.hidden = true;
    m_PathEditor.textColor = NSColor.labelColor;
    m_PathEditor.toolTip = nil;
    m_RequestErrorMessage = nil;
    m_LocationGeneration.reset();
    m_PresentationState.reset();
    m_SegmentTargets = [NSMutableArray new];
    for( NSView *view in [m_PathStack.arrangedSubviews copy] ) {
        [m_PathStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    m_PathStack.hidden = true;
    m_FallbackLabel.hidden = false;
    m_FallbackLabel.stringValue = @"";
    self.accessibilityValue = @"";
    [m_BusyIndicator stopAnimation:nil];
    [self applyVisibleErrorText:nil priority:core::VisualPriority::Normal];
    m_Panel = _panel;
}

- (void)buildLayout
{
    NSImage *const folder_image =
        [[NSImage imageWithSystemSymbolName:@"folder.fill" accessibilityDescription:nil]
            imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:15.0
                                                                                           weight:NSFontWeightRegular]];
    m_LocationButton = [NSButton buttonWithImage:folder_image
                                          target:self
                                          action:@selector(onEditPath:)];
    m_LocationButton.bezelStyle = NSBezelStyleInline;
    m_LocationButton.bordered = false;
    m_LocationButton.controlSize = NSControlSizeRegular;
    m_LocationButton.imageScaling = NSImageScaleProportionallyDown;
    m_LocationButton.toolTip = NSLocalizedString(@"Edit path (Command-L)", "Explorer address bar");
    m_LocationButton.accessibilityLabel = NSLocalizedString(@"Edit path", "Explorer accessibility label");

    m_PathStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    m_PathStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    m_PathStack.alignment = NSLayoutAttributeCenterY;
    m_PathStack.spacing = 0.0;

    m_PathEditor = [[NSComboBox alloc] initWithFrame:NSZeroRect];
    m_PathEditor.hidden = true;
    m_PathEditor.usesDataSource = true;
    m_PathEditor.dataSource = self;
    m_PathEditor.delegate = self;
    m_PathEditor.completes = true;
    m_PathEditor.controlSize = NSControlSizeRegular;
    m_PathEditor.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    m_PathEditor.placeholderString = NSLocalizedString(@"Enter a path", "Explorer address bar");
    m_PathEditor.target = self;
    m_PathEditor.action = @selector(onPathEditorCommit:);
    m_PathEditor.accessibilityLabel = NSLocalizedString(@"Path", "Explorer accessibility label");

    m_FallbackLabel = [NSTextField labelWithString:@""];
    m_FallbackLabel.hidden = true;
    m_FallbackLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    m_FallbackLabel.font = [NSFont systemFontOfSize:NSFont.systemFontSize];

    m_ErrorLabel = [NSTextField labelWithString:@""];
    m_ErrorLabel.hidden = true;
    m_ErrorLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    m_ErrorLabel.maximumNumberOfLines = 1;
    m_ErrorLabel.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    m_ErrorLabel.textColor = NSColor.systemRedColor;
    m_ErrorLabel.accessibilityLabel = NSLocalizedString(@"Folder status", "Explorer accessibility label");

    m_FindField = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    m_FindField.controlSize = NSControlSizeRegular;
    m_FindField.placeholderString = NSLocalizedString(@"Find Files", "Explorer toolbar");
    m_FindField.toolTip = NSLocalizedString(@"Open Find Files with this filename mask", "Explorer toolbar");
    m_FindField.target = self;
    m_FindField.action = @selector(onFindFiles:);
    m_FindField.sendsWholeSearchString = true;
    m_FindField.accessibilityLabel = NSLocalizedString(@"Find Files", "Explorer accessibility label");

    m_BusyIndicator = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    m_BusyIndicator.style = NSProgressIndicatorStyleSpinning;
    m_BusyIndicator.controlSize = NSControlSizeSmall;
    m_BusyIndicator.displayedWhenStopped = false;
    m_BusyIndicator.indeterminate = true;

    for( NSView *view in
         @[m_LocationButton, m_PathStack, m_PathEditor, m_FallbackLabel, m_ErrorLabel, m_FindField, m_BusyIndicator] ) {
        view.translatesAutoresizingMaskIntoConstraints = false;
        [self addSubview:view];
    }

    [m_LocationButton setContentHuggingPriority:NSLayoutPriorityRequired
                                 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_FindField setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_ErrorLabel setContentHuggingPriority:NSLayoutPriorityRequired
                             forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_ErrorLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [m_LocationButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
        [m_LocationButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_LocationButton.widthAnchor constraintEqualToConstant:g_InnerControlHeight],
        [m_LocationButton.heightAnchor constraintEqualToConstant:g_InnerControlHeight],

        [m_PathStack.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:2.0],
        [m_PathStack.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_PathStack.trailingAnchor constraintLessThanOrEqualToAnchor:m_ErrorLabel.leadingAnchor constant:-6.0],
        [m_PathStack.heightAnchor constraintGreaterThanOrEqualToConstant:g_InnerControlHeight],

        [m_PathEditor.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:2.0],
        [m_PathEditor.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_PathEditor.trailingAnchor constraintEqualToAnchor:m_ErrorLabel.leadingAnchor constant:-6.0],
        [m_PathEditor.heightAnchor constraintEqualToConstant:g_InnerControlHeight],

        [m_FallbackLabel.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:4.0],
        [m_FallbackLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_FallbackLabel.trailingAnchor constraintEqualToAnchor:m_ErrorLabel.leadingAnchor constant:-6.0],

        [m_ErrorLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_ErrorLabel.trailingAnchor constraintEqualToAnchor:m_BusyIndicator.leadingAnchor constant:-4.0],
        [m_ErrorLabel.widthAnchor constraintLessThanOrEqualToConstant:220.0],

        [m_BusyIndicator.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_BusyIndicator.widthAnchor constraintEqualToConstant:16.0],
        [m_BusyIndicator.heightAnchor constraintEqualToConstant:16.0],
        [m_BusyIndicator.trailingAnchor constraintEqualToAnchor:m_FindField.leadingAnchor constant:-8.0],

        [m_FindField.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_FindField.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2.0],
        [m_FindField.widthAnchor constraintEqualToConstant:g_SearchFieldWidth],
        [m_FindField.heightAnchor constraintEqualToConstant:g_InnerControlHeight],
    ]];
}

- (NSProgressIndicator *)busyIndicator
{
    return m_BusyIndicator;
}

- (NSTextField *)errorLabel
{
    return m_ErrorLabel;
}

- (NSString *)requestErrorMessage
{
    return m_RequestErrorMessage;
}

- (void)applyVisibleErrorText:(NSString *)_error_text priority:(const core::VisualPriority)_priority
{
    const bool changed = m_ErrorLabel.hidden == (_error_text.length != 0) ||
                         ![m_ErrorLabel.stringValue isEqualToString:_error_text ?: @""];
    m_ErrorLabel.hidden = _error_text.length == 0;
    m_ErrorLabel.stringValue = _error_text ?: @"";
    m_ErrorLabel.toolTip = _error_text;
    m_ErrorLabel.accessibilityValue = _error_text;
    m_ErrorLabel.accessibilityHelp = _error_text;
    self.accessibilityHelp = _error_text;
    m_ErrorLabel.textColor =
        _priority >= core::VisualPriority::Blocking ? NSColor.systemRedColor : NSColor.systemOrangeColor;
    if( changed && _error_text.length != 0 )
        NSAccessibilityPostNotification(m_ErrorLabel, NSAccessibilityValueChangedNotification);
}

- (void)applyPaneSnapshot:(const core::PaneSnapshot &)_snapshot
{
    dispatch_assert_queue(dispatch_get_main_queue());

    const core::PaneState &state = _snapshot.state;
    const bool location_generation_changed =
        !m_LocationGeneration || *m_LocationGeneration != state.location_generation;
    if( !state.visible_error && (state.load_phase == core::PaneLoadPhase::Loading ||
                                 state.load_phase == core::PaneLoadPhase::Refreshing || location_generation_changed) ) {
        m_RequestErrorMessage = nil;
        m_PathEditor.textColor = NSColor.labelColor;
        m_PathEditor.toolTip = nil;
    }
    const core::PaneVisualState visual = core::VisualStateMapper::MapPane(_snapshot);

    const core::VisualMessage *visible_message = nullptr;
    if( visual.status.kind == core::PaneStatusVisualKind::Error && visual.status.message )
        visible_message = std::addressof(*visual.status.message);
    else if( visual.nonblocking_notice )
        visible_message = std::addressof(*visual.nonblocking_notice);
    NSString *const snapshot_error_text = visible_message != nullptr ? LocalizedVisualMessage(*visible_message) : nil;
    [self applyVisibleErrorText:snapshot_error_text ?: m_RequestErrorMessage priority:visual.priority];

    if( visual.breadcrumb.shows_activity )
        [m_BusyIndicator startAnimation:nil];
    else
        [m_BusyIndicator stopAnimation:nil];

    BreadcrumbPresentationState presentation_state{
        .load_phase = state.load_phase,
        .address_editable = visual.breadcrumb.editable,
        .is_uniform = state.is_uniform,
        .path = state.path,
        .display_title = state.display_title,
        .host = state.host,
    };
    const bool address_context_changed =
        !m_PresentationState || !HasSameAddressContext(*m_PresentationState, presentation_state);
    const bool breadcrumb_content_changed =
        !m_PresentationState || !HasSameBreadcrumbContent(*m_PresentationState, presentation_state);
    const bool location_changed = location_generation_changed || address_context_changed;
    if( location_changed ) {
        m_LocationGeneration = state.location_generation;
        ++m_PathGeneration;
        ++m_NavigationGeneration;
    }

    const bool editor_visible = !m_PathEditor.hidden;
    m_PresentationState = std::move(presentation_state);
    m_LocationButton.enabled = m_PresentationState->address_editable;

    if( editor_visible && (location_changed || !m_PresentationState->address_editable) )
        [self leaveEditingModeFocusingPanel:true];
    else if( editor_visible )
        return;

    if( !breadcrumb_content_changed && !location_changed )
        return;

    NSString *const display_title = [NSString stringWithUTF8String:state.display_title.c_str()] ?: @"";

    if( !state.is_uniform ) {
        m_PathStack.hidden = true;
        m_FallbackLabel.hidden = false;
        m_FallbackLabel.stringValue = display_title.length == 0
                                          ? NSLocalizedString(@"Multiple Locations", "Explorer address fallback")
                                          : display_title;
        self.accessibilityValue = m_FallbackLabel.stringValue;
        NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
        return;
    }

    if( !HasAddressLocation(*m_PresentationState) ) {
        m_PathStack.hidden = true;
        m_FallbackLabel.hidden = false;
        m_FallbackLabel.stringValue = display_title;
        self.accessibilityValue = display_title;
        NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
        return;
    }

    m_FallbackLabel.hidden = true;
    m_PathStack.hidden = false;
    [self rebuildSegmentsForPath:state.path host:state.host];
    self.accessibilityValue = [NSString stringWithUTF8String:state.path.c_str()] ?: @"";
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

- (void)rebuildSegmentsForPath:(const std::string &)_path host:(const VFSHostPtr &)_host
{
    for( NSView *view in [m_PathStack.arrangedSubviews copy] ) {
        [m_PathStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    m_SegmentTargets = [NSMutableArray new];

    if( !_host )
        return;

    NSString *root_title = @"/";
    if( _host->IsNativeFS() ) {
        NSString *const display_name = [NSFileManager.defaultManager displayNameAtPath:@"/"];
        if( display_name.length )
            root_title = display_name;
    }
    else if( _host->Tag() ) {
        root_title = [NSString stringWithUTF8String:_host->Tag()];
    }

    [self appendSegment:[[NCExplorerBreadcrumbTarget alloc] initWithTitle:root_title path:"/" host:_host]];

    std::string accumulated = "/";
    size_t offset = 0;
    while( offset < _path.size() ) {
        while( offset < _path.size() && _path[offset] == '/' )
            ++offset;
        if( offset == _path.size() )
            break;
        const size_t separator = _path.find('/', offset);
        const size_t end = separator == std::string::npos ? _path.size() : separator;
        const std::string component = _path.substr(offset, end - offset);
        accumulated += component + "/";
        NSString *const title = [NSString stringWithUTF8String:component.c_str()];
        [self appendSegment:[[NCExplorerBreadcrumbTarget alloc] initWithTitle:title path:accumulated host:_host]];
        offset = end;
    }
}

- (void)appendSegment:(NCExplorerBreadcrumbTarget *)_target
{
    const NSInteger target_index = static_cast<NSInteger>(m_SegmentTargets.count);
    [m_SegmentTargets addObject:_target];
    NSButton *const segment = [NSButton buttonWithTitle:_target.title target:self action:@selector(onSegmentClicked:)];
    segment.bezelStyle = NSBezelStyleInline;
    segment.bordered = false;
    segment.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    segment.lineBreakMode = NSLineBreakByTruncatingMiddle;
    segment.tag = target_index;
    segment.toolTip = [NSString stringWithUTF8String:_target.path.c_str()];
    segment.accessibilityLabel = _target.title;
    [segment.heightAnchor constraintEqualToConstant:g_InnerControlHeight].active = true;
    [segment setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_PathStack addArrangedSubview:segment];

    NSImage *const chevron =
        [[NSImage imageWithSystemSymbolName:@"chevron.right" accessibilityDescription:nil]
            imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:11.0
                                                                                           weight:NSFontWeightSemibold]];
    NSButton *const siblings = [NSButton buttonWithImage:chevron
                                                  target:self
                                                  action:@selector(onSiblingMenu:)];
    siblings.bezelStyle = NSBezelStyleInline;
    siblings.bordered = false;
    siblings.imageScaling = NSImageScaleProportionallyDown;
    siblings.tag = target_index;
    siblings.toolTip = NSLocalizedString(@"Show sibling folders", "Explorer address bar");
    siblings.accessibilityLabel = NSLocalizedString(@"Show sibling folders", "Explorer accessibility label");
    [siblings.widthAnchor constraintEqualToConstant:g_InnerControlHeight].active = true;
    [siblings.heightAnchor constraintEqualToConstant:g_InnerControlHeight].active = true;
    [m_PathStack addArrangedSubview:siblings];
}

- (void)onSegmentClicked:(NSButton *)_sender
{
    NCExplorerBreadcrumbTarget *const target = [self targetForButton:_sender];
    if( target )
        [self navigateToPath:target.path host:target.host];
}

- (void)onSiblingMenu:(NSButton *)_sender
{
    NCExplorerBreadcrumbTarget *const target = [self targetForButton:_sender];
    if( !target )
        return;

    NSMenu *const menu = [NSMenu new];
    NSMenuItem *const loading = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Loading…", "Explorer address bar")
                                                           action:nil
                                                    keyEquivalent:@""];
    loading.enabled = false;
    [menu addItem:loading];

    const unsigned long generation = m_PathGeneration;
    const VFSHostPtr host = target.host;
    std::string parent = target.path;
    if( parent != "/" ) {
        if( parent.back() == '/' )
            parent.pop_back();
        const size_t slash = parent.find_last_of('/');
        parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    }

    __weak NCExplorerBreadcrumbControl *weak_self = self;
    dispatch_to_background([weak_self, menu, host, parent, generation] {
        std::vector<std::string> directories;
        const auto listing = host->FetchDirectoryListing(parent, VFSFlags::F_NoDotDot);
        if( listing ) {
            for( unsigned index = 0; index != (*listing)->Count(); ++index )
                if( (*listing)->IsDir(index) && !(*listing)->IsDotDot(index) )
                    directories.emplace_back((*listing)->Filename(index));
        }
        std::ranges::sort(directories, [](const std::string &_lhs, const std::string &_rhs) {
            return strcasecmp(_lhs.c_str(), _rhs.c_str()) < 0;
        });

        dispatch_to_main_queue([weak_self, menu, host, parent, generation, directories = std::move(directories)] {
            NCExplorerBreadcrumbControl *const strong_self = weak_self;
            if( !strong_self || strong_self->m_PathGeneration != generation )
                return;
            [menu removeAllItems];
            if( directories.empty() ) {
                NSMenuItem *const empty =
                    [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"No folders", "Explorer address bar")
                                               action:nil
                                        keyEquivalent:@""];
                empty.enabled = false;
                [menu addItem:empty];
                return;
            }
            for( const std::string &name : directories ) {
                NSString *const title = [NSString stringWithUTF8String:name.c_str()];
                const std::string path = EnsureTrailingSlash(parent + name);
                NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:title
                                                                    action:@selector(onSiblingSelected:)
                                                             keyEquivalent:@""];
                item.target = strong_self;
                item.representedObject = [[NCExplorerBreadcrumbTarget alloc] initWithTitle:title path:path host:host];
                [menu addItem:item];
            }
        });
    });

    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0.0, NSMinY(_sender.bounds)) inView:_sender];
}

- (NCExplorerBreadcrumbTarget *)targetForButton:(NSButton *)_button
{
    const NSInteger index = _button.tag;
    return index >= 0 && index < static_cast<NSInteger>(m_SegmentTargets.count) ? m_SegmentTargets[index] : nil;
}

- (void)onSiblingSelected:(NSMenuItem *)_sender
{
    NCExplorerBreadcrumbTarget *const target = _sender.representedObject;
    if( target )
        [self navigateToPath:target.path host:target.host];
}

- (void)focusAddressField
{
    if( !m_PresentationState || !IsAddressEditable(*m_PresentationState) ) {
        NSBeep();
        return;
    }

    ++m_NavigationGeneration;
    ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    m_PathStack.hidden = true;
    m_FallbackLabel.hidden = true;
    m_PathEditor.hidden = false;
    m_PathEditor.textColor = NSColor.labelColor;
    m_PathEditor.toolTip = nil;
    m_PathEditor.stringValue = [NSString stringWithUTF8String:m_PresentationState->path.c_str()];
    [self.window makeFirstResponder:m_PathEditor];
    [m_PathEditor selectText:nil];
}

- (IBAction)ToggleViewHiddenFiles:(id)_sender
{
    NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
    if( dispatcher )
        [dispatcher ToggleViewHiddenFiles:_sender];
    else
        NSBeep();
}

- (BOOL)validateMenuItem:(NSMenuItem *)_item
{
    if( _item.action == @selector(ToggleViewHiddenFiles:) ) {
        NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
        return dispatcher ? [dispatcher validateMenuItem:_item] : false;
    }
    return true;
}

- (void)restoreAddressPresentation
{
    const bool show_path = m_PresentationState && HasAddressLocation(*m_PresentationState);
    m_PathStack.hidden = !show_path;
    m_FallbackLabel.hidden = show_path;
}

- (void)leaveEditingModeFocusingPanel:(const bool)_focus_panel
{
    NSResponder *const first_responder = self.window.firstResponder;
    const bool editor_had_focus = first_responder == m_PathEditor || first_responder == m_PathEditor.currentEditor;
    ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    m_CompletionCancellation.reset();
    m_PathEditor.hidden = true;
    m_Completions = @[];
    [m_PathEditor reloadData];
    [self restoreAddressPresentation];
    if( _focus_panel && editor_had_focus )
        [self.window makeFirstResponder:m_Panel.view];
}

- (void)onEditPath:(id) [[maybe_unused]] _sender
{
    [self focusAddressField];
}

- (void)onPathEditorCommit:(id) [[maybe_unused]] _sender
{
    if( !m_PresentationState || !IsAddressEditable(*m_PresentationState) )
        return;
    const std::string expanded =
        ExpandAddressPath(m_PathEditor.stringValue.fileSystemRepresentation, *m_PresentationState);
    if( expanded.empty() ) {
        NSBeep();
        return;
    }
    [self navigateToPath:EnsureTrailingSlash(expanded) host:m_PresentationState->host];
}

- (void)navigateToPath:(const std::string &)_path host:(const VFSHostPtr &)_host
{
    const unsigned long generation = ++m_NavigationGeneration;
    m_RequestErrorMessage = nil;
    m_PathEditor.textColor = NSColor.labelColor;
    m_PathEditor.toolTip = nil;
    [self applyVisibleErrorText:nil priority:core::VisualPriority::Normal];
    auto request = std::make_shared<panel::DirectoryChangeRequest>();
    request->RequestedDirectory = EnsureTrailingSlash(_path);
    request->VFS = _host;
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    core::FileManagerErrorContext error_context;
    error_context.affected_items.emplace_back(request->RequestedDirectory);
    if( const char *const provider = _host ? _host->Tag() : nullptr; provider != nullptr )
        error_context.provider_id = provider;
    __weak NCExplorerBreadcrumbControl *weak_self = self;
    request->LoadingResultCallback = [weak_self, generation, error_context = std::move(error_context)](
                                         const std::expected<void, Error> &_result,
                                         const panel::DirectoryChangeResultSource _source,
                                         const std::function<bool()> &_is_current) {
        if( _result )
            return;
        NSString *const error_text = NavigationRequestErrorText(_result.error(), _source, error_context);
        if( error_text.length == 0 )
            return;
        dispatch_or_run_in_main_queue([weak_self, generation, is_current = _is_current, error_text] {
            if( !is_current() )
                return;
            if( NCExplorerBreadcrumbControl *const strong_self = weak_self;
                strong_self && strong_self->m_NavigationGeneration == generation ) {
                strong_self->m_RequestErrorMessage = [error_text copy];
                strong_self->m_PathEditor.textColor = NSColor.systemRedColor;
                strong_self->m_PathEditor.toolTip = error_text;
                [strong_self applyVisibleErrorText:error_text priority:core::VisualPriority::Blocking];
                NSBeep();
            }
        });
    };
    [m_Panel GoToDirWithContext:request];
}

- (void)controlTextDidChange:(NSNotification *)_notification
{
    if( _notification.object != m_PathEditor || !m_PresentationState || !m_PresentationState->is_uniform ||
        !m_PresentationState->host )
        return;

    ++m_NavigationGeneration;
    const unsigned long generation = ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_CompletionCancellation = cancellation;
    m_Completions = @[];
    [m_PathEditor reloadData];
    const BreadcrumbPresentationState context = *m_PresentationState;
    const std::string expanded = ExpandAddressPath(m_PathEditor.stringValue.fileSystemRepresentation, context);
    if( expanded.empty() )
        return;

    std::string parent = expanded;
    std::string prefix;
    if( parent.back() != '/' ) {
        const size_t slash = parent.find_last_of('/');
        prefix = slash == std::string::npos ? parent : parent.substr(slash + 1);
        parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    }
    const VFSHostPtr host = context.host;
    __weak NCExplorerBreadcrumbControl *weak_self = self;
    dispatch_to_main_queue_after(150ms, [weak_self, host, parent, prefix, generation, cancellation] {
        NCExplorerBreadcrumbControl *const strong_self = weak_self;
        if( !strong_self || cancellation->load() || strong_self->m_CompletionGeneration != generation ||
            strong_self->m_PathEditor.hidden )
            return;

        dispatch_to_background([weak_self, host, parent, prefix, generation, cancellation] {
            std::vector<std::string> completions;
            const auto listing = host->FetchDirectoryListing(
                parent, VFSFlags::F_NoDotDot, [cancellation] { return cancellation->load(); });
            if( listing && !cancellation->load() ) {
                for( unsigned index = 0; index != (*listing)->Count(); ++index ) {
                    if( !(*listing)->IsDir(index) || (*listing)->IsDotDot(index) )
                        continue;
                    const std::string &name = (*listing)->Filename(index);
                    if( prefix.empty() || strncasecmp(name.c_str(), prefix.c_str(), prefix.size()) == 0 )
                        completions.emplace_back(EnsureTrailingSlash(parent + name));
                }
            }
            std::ranges::sort(completions, [](const std::string &_lhs, const std::string &_rhs) {
                return strcasecmp(_lhs.c_str(), _rhs.c_str()) < 0;
            });

            dispatch_to_main_queue([weak_self, generation, cancellation, completions = std::move(completions)] {
                NCExplorerBreadcrumbControl *const strong_self = weak_self;
                if( !strong_self || cancellation->load() || strong_self->m_CompletionGeneration != generation ||
                    strong_self->m_PathEditor.hidden )
                    return;
                NSMutableArray<NSString *> *const values = [NSMutableArray arrayWithCapacity:completions.size()];
                for( const std::string &completion : completions )
                    [values addObject:[NSString stringWithUTF8String:completion.c_str()]];
                strong_self->m_Completions = values;
                [strong_self->m_PathEditor reloadData];
            });
        });
    });
}

- (BOOL)control:(NSControl *)_control
               textView:(NSTextView *) [[maybe_unused]] _text_view
    doCommandBySelector:(SEL)_selector
{
    if( _control == m_PathEditor && _selector == @selector(cancelOperation:) ) {
        [self leaveEditingModeFocusingPanel:true];
        return true;
    }
    return false;
}

- (void)controlTextDidEndEditing:(NSNotification *)_notification
{
    if( _notification.object != m_PathEditor )
        return;

    // The delegate notification can arrive while AppKit is still handing focus to the clicked
    // control. Defer only the visual teardown so the new responder remains authoritative.
    __weak NCExplorerBreadcrumbControl *weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      NCExplorerBreadcrumbControl *const strong_self = weak_self;
      if( !strong_self || strong_self->m_PathEditor.hidden )
          return;
      NSResponder *const responder = strong_self.window.firstResponder;
      if( responder == strong_self->m_PathEditor || responder == strong_self->m_PathEditor.currentEditor )
          return;
      [strong_self leaveEditingModeFocusingPanel:false];
    });
}

- (NSInteger)numberOfItemsInComboBox:(NSComboBox *) [[maybe_unused]] _combo_box
{
    return m_Completions.count;
}

- (id)comboBox:(NSComboBox *) [[maybe_unused]] _combo_box objectValueForItemAtIndex:(NSInteger)_index
{
    return _index >= 0 && _index < static_cast<NSInteger>(m_Completions.count) ? m_Completions[_index] : nil;
}

- (NSString *)comboBox:(NSComboBox *) [[maybe_unused]] _combo_box completedString:(NSString *)_string
{
    const std::string expanded = [m_Panel expandPath:_string.fileSystemRepresentation];
    NSString *const normalized_prefix = [NSString stringWithUTF8String:expanded.c_str()];
    for( NSString *candidate in m_Completions )
        if( [candidate rangeOfString:normalized_prefix options:NSAnchoredSearch | NSCaseInsensitiveSearch].location !=
            NSNotFound )
            return candidate;
    return nil;
}

- (void)onFindFiles:(NSSearchField *)_sender
{
    if( _sender.stringValue.length == 0 )
        return;
    if( [m_Panel.view.actionsDispatcher validateActionBySelector:@selector(onMainMenuPerformFindAction:)] )
        [m_Panel.view.actionsDispatcher onMainMenuPerformFindAction:_sender];
    else
        NSBeep();
}

- (void)mouseDown:(NSEvent *) [[maybe_unused]] _event
{
    [self focusAddressField];
}

@end
