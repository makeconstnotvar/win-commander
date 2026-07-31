// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerBreadcrumbControl.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelController+DataAccess.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../FilePanels/PanelView.h"
#include <Base/dispatch_cpp.h>
#include <Panel/PanelData.h>
#include <Utility/PathManip.h>
#include <VFS/VFS.h>

#include <algorithm>
#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

using namespace nc;

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

@interface NCExplorerBreadcrumbControl () <NSComboBoxDataSource, NSComboBoxDelegate, NSTextFieldDelegate>
@end

@implementation NCExplorerBreadcrumbControl {
    PanelController *m_Panel;
    NSButton *m_LocationButton;
    NSStackView *m_PathStack;
    NSComboBox *m_PathEditor;
    NSTextField *m_FallbackLabel;
    NSSearchField *m_FindField;
    NSProgressIndicator *m_BusyIndicator;
    NSMutableArray<NCExplorerBreadcrumbTarget *> *m_SegmentTargets;
    NSArray<NSString *> *m_Completions;
    std::shared_ptr<std::atomic_bool> m_CompletionCancellation;
    unsigned long m_PathGeneration;
    unsigned long m_CompletionGeneration;
    unsigned long m_NavigationGeneration;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityLabel = NSLocalizedString(@"Address bar", "Explorer accessibility label");
        [self buildLayout];
        [self panelPathChanged];
    }
    return self;
}

- (void)buildLayout
{
    m_LocationButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"folder.fill"
                                                                 accessibilityDescription:nil]
                                         target:self
                                         action:@selector(onEditPath:)];
    m_LocationButton.bezelStyle = NSBezelStyleInline;
    m_LocationButton.bordered = false;
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
    m_PathEditor.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    m_PathEditor.placeholderString = NSLocalizedString(@"Enter a path", "Explorer address bar");
    m_PathEditor.target = self;
    m_PathEditor.action = @selector(onPathEditorCommit:);
    m_PathEditor.accessibilityLabel = NSLocalizedString(@"Path", "Explorer accessibility label");

    m_FallbackLabel = [NSTextField labelWithString:@""];
    m_FallbackLabel.hidden = true;
    m_FallbackLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    m_FallbackLabel.font = [NSFont systemFontOfSize:NSFont.systemFontSize];

    m_FindField = [[NSSearchField alloc] initWithFrame:NSZeroRect];
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

    for( NSView *view in @[m_LocationButton, m_PathStack, m_PathEditor, m_FallbackLabel, m_FindField, m_BusyIndicator] ) {
        view.translatesAutoresizingMaskIntoConstraints = false;
        [self addSubview:view];
    }

    [m_LocationButton setContentHuggingPriority:NSLayoutPriorityRequired
                                 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_FindField setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [m_LocationButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2.0],
        [m_LocationButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_LocationButton.widthAnchor constraintEqualToConstant:24.0],

        [m_PathStack.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:2.0],
        [m_PathStack.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_PathStack.trailingAnchor constraintLessThanOrEqualToAnchor:m_BusyIndicator.leadingAnchor constant:-6.0],

        [m_PathEditor.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:2.0],
        [m_PathEditor.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_PathEditor.trailingAnchor constraintEqualToAnchor:m_BusyIndicator.leadingAnchor constant:-6.0],

        [m_FallbackLabel.leadingAnchor constraintEqualToAnchor:m_LocationButton.trailingAnchor constant:4.0],
        [m_FallbackLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_FallbackLabel.trailingAnchor constraintEqualToAnchor:m_BusyIndicator.leadingAnchor constant:-6.0],

        [m_BusyIndicator.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_BusyIndicator.widthAnchor constraintEqualToConstant:16.0],
        [m_BusyIndicator.heightAnchor constraintEqualToConstant:16.0],
        [m_BusyIndicator.trailingAnchor constraintEqualToAnchor:m_FindField.leadingAnchor constant:-8.0],

        [m_FindField.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_FindField.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2.0],
        [m_FindField.widthAnchor constraintEqualToConstant:180.0],
    ]];
}

- (NSProgressIndicator *)busyIndicator
{
    return m_BusyIndicator;
}

- (void)panelPathChanged
{
    ++m_PathGeneration;
    ++m_NavigationGeneration;
    [self leaveEditingMode];

    if( !m_Panel.isUniform ) {
        m_PathStack.hidden = true;
        m_FallbackLabel.hidden = false;
        const std::string &title = m_Panel.data.Listing().Title();
        m_FallbackLabel.stringValue = title.empty()
                                          ? NSLocalizedString(@"Multiple Locations", "Explorer address fallback")
                                          : [NSString stringWithUTF8String:title.c_str()];
        return;
    }

    m_FallbackLabel.hidden = true;
    m_PathStack.hidden = false;
    [self rebuildSegmentsForPath:m_Panel.currentDirectoryPath host:m_Panel.vfs];
}

- (void)rebuildSegmentsForPath:(const std::string &)_path host:(const VFSHostPtr &)_host
{
    for( NSView *view in [m_PathStack.arrangedSubviews copy] ) {
        [m_PathStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    m_SegmentTargets = [NSMutableArray new];

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
    [segment setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    [m_PathStack addArrangedSubview:segment];

    NSButton *const siblings = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"chevron.right"
                                                                             accessibilityDescription:nil]
                                                   target:self
                                                   action:@selector(onSiblingMenu:)];
    siblings.bezelStyle = NSBezelStyleInline;
    siblings.bordered = false;
    siblings.imageScaling = NSImageScaleProportionallyDown;
    siblings.tag = target_index;
    siblings.toolTip = NSLocalizedString(@"Show sibling folders", "Explorer address bar");
    siblings.accessibilityLabel = NSLocalizedString(@"Show sibling folders", "Explorer accessibility label");
    [siblings.widthAnchor constraintEqualToConstant:16.0].active = true;
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
                NSMenuItem *const empty = [[NSMenuItem alloc]
                    initWithTitle:NSLocalizedString(@"No folders", "Explorer address bar")
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
    if( !m_Panel.isUniform ) {
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
    m_PathEditor.stringValue = [NSString stringWithUTF8String:m_Panel.currentDirectoryPath.c_str()];
    [self.window makeFirstResponder:m_PathEditor];
    [m_PathEditor selectText:nil];
}

- (void)leaveEditingMode
{
    ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    m_CompletionCancellation.reset();
    m_PathEditor.hidden = true;
    m_Completions = @[];
    [m_PathEditor reloadData];
}

- (void)onEditPath:(id) [[maybe_unused]] _sender
{
    [self focusAddressField];
}

- (void)onPathEditorCommit:(id) [[maybe_unused]] _sender
{
    if( !m_Panel.isUniform )
        return;
    const std::string expanded = [m_Panel expandPath:m_PathEditor.stringValue.fileSystemRepresentation];
    if( expanded.empty() ) {
        NSBeep();
        return;
    }
    [self navigateToPath:EnsureTrailingSlash(expanded) host:m_Panel.vfs];
}

- (void)navigateToPath:(const std::string &)_path host:(const VFSHostPtr &)_host
{
    const unsigned long generation = ++m_NavigationGeneration;
    auto request = std::make_shared<panel::DirectoryChangeRequest>();
    request->RequestedDirectory = EnsureTrailingSlash(_path);
    request->VFS = _host;
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    __weak NCExplorerBreadcrumbControl *weak_self = self;
    request->LoadingResultCallback = [weak_self, generation](const std::expected<void, Error> &_result) {
        if( _result )
            return;
        dispatch_to_main_queue([weak_self, generation] {
            if( NCExplorerBreadcrumbControl *const strong_self = weak_self;
                strong_self && strong_self->m_NavigationGeneration == generation ) {
                strong_self->m_PathEditor.textColor = NSColor.systemRedColor;
                strong_self->m_PathEditor.toolTip = NSLocalizedString(@"The folder could not be opened", "Explorer address bar");
                NSBeep();
            }
        });
    };
    [m_Panel GoToDirWithContext:request];
}

- (void)controlTextDidChange:(NSNotification *)_notification
{
    if( _notification.object != m_PathEditor || !m_Panel.isUniform )
        return;

    ++m_NavigationGeneration;
    const unsigned long generation = ++m_CompletionGeneration;
    if( m_CompletionCancellation )
        m_CompletionCancellation->store(true);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_CompletionCancellation = cancellation;
    m_Completions = @[];
    [m_PathEditor reloadData];
    const std::string expanded = [m_Panel expandPath:m_PathEditor.stringValue.fileSystemRepresentation];
    if( expanded.empty() )
        return;

    std::string parent = expanded;
    std::string prefix;
    if( parent.back() != '/' ) {
        const size_t slash = parent.find_last_of('/');
        prefix = slash == std::string::npos ? parent : parent.substr(slash + 1);
        parent = slash == std::string::npos ? "/" : parent.substr(0, slash + 1);
    }
    const VFSHostPtr host = m_Panel.vfs;
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
        [self leaveEditingMode];
        m_PathStack.hidden = !m_Panel.isUniform;
        m_FallbackLabel.hidden = m_Panel.isUniform;
        [self.window makeFirstResponder:m_Panel.view];
        return true;
    }
    return false;
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
        if( [candidate rangeOfString:normalized_prefix
                             options:NSAnchoredSearch | NSCaseInsensitiveSearch].location != NSNotFound )
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
