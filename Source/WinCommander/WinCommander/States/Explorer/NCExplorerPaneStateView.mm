// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerPaneStateView.h"

#include <algorithm>

using nc::core::PaneVisualKind;
using nc::core::PaneVisualState;

namespace {

bool IsErrorKind(const PaneVisualKind _kind) noexcept
{
    switch( _kind ) {
        case PaneVisualKind::PermissionBlocked:
        case PaneVisualKind::PathNotFound:
        case PaneVisualKind::VolumeDisconnected:
        case PaneVisualKind::RemoteUnavailable:
        case PaneVisualKind::Unsupported:
        case PaneVisualKind::Error:
            return true;
        case PaneVisualKind::Unavailable:
        case PaneVisualKind::Loading:
        case PaneVisualKind::Refreshing:
        case PaneVisualKind::Ready:
        case PaneVisualKind::EmptyFolder:
            return false;
    }
}

NSString *FallbackMessage(const PaneVisualKind _kind)
{
    switch( _kind ) {
        case PaneVisualKind::Unavailable:
            return @"";
        case PaneVisualKind::Loading:
            return NSLocalizedString(@"Loading…", "Explorer loading pane state");
        case PaneVisualKind::EmptyFolder:
            return NSLocalizedString(@"Folder is empty.", "Explorer empty-folder pane state");
        case PaneVisualKind::PermissionBlocked:
            return NSLocalizedString(@"Permission denied.", "Explorer permission-denied pane state");
        case PaneVisualKind::PathNotFound:
            return NSLocalizedString(@"Folder was not found.", "Explorer missing-folder pane state");
        case PaneVisualKind::VolumeDisconnected:
            return NSLocalizedString(@"Drive is disconnected.", "Explorer disconnected-volume pane state");
        case PaneVisualKind::RemoteUnavailable:
            return NSLocalizedString(@"Remote location is unavailable.", "Explorer unavailable-remote pane state");
        case PaneVisualKind::Unsupported:
            return NSLocalizedString(@"This location is not supported.", "Explorer unsupported-location pane state");
        case PaneVisualKind::Error:
            return NSLocalizedString(@"Unable to show this folder.", "Explorer generic-error pane state");
        case PaneVisualKind::Refreshing:
        case PaneVisualKind::Ready:
            return @"";
    }
}

NSString *StringFromUTF8(const std::string &_value)
{
    return [[NSString alloc] initWithBytes:_value.data() length:_value.size() encoding:NSUTF8StringEncoding];
}

NSString *LocalizedVisualMessage(const nc::core::VisualMessage &_message)
{
    NSString *const fallback = StringFromUTF8(_message.user_message_fallback) ?: @"";
    NSString *const key = StringFromUTF8(_message.user_message_key);
    if( key.length == 0 )
        return fallback;

    NSString *const value = [NSBundle.mainBundle localizedStringForKey:key value:fallback table:nil];
    return value.length == 0 || ([value isEqualToString:key] && fallback.length != 0) ? fallback : value;
}

NSString *Message(const PaneVisualState &_state)
{
    if( _state.status.message ) {
        NSString *const localized = LocalizedVisualMessage(*_state.status.message);
        if( localized.length != 0 )
            return localized;
    }
    return FallbackMessage(_state.kind);
}

NSString *SymbolName(const PaneVisualKind _kind)
{
    switch( _kind ) {
        case PaneVisualKind::Unavailable:
            return @"questionmark.folder";
        case PaneVisualKind::EmptyFolder:
            return @"folder";
        case PaneVisualKind::PermissionBlocked:
            return @"lock.fill";
        case PaneVisualKind::PathNotFound:
            return @"folder.badge.questionmark";
        case PaneVisualKind::VolumeDisconnected:
            return @"externaldrive.badge.exclamationmark";
        case PaneVisualKind::RemoteUnavailable:
            return @"network.slash";
        case PaneVisualKind::Unsupported:
            return @"nosign";
        case PaneVisualKind::Error:
            return @"exclamationmark.triangle.fill";
        case PaneVisualKind::Loading:
        case PaneVisualKind::Refreshing:
        case PaneVisualKind::Ready:
            return @"";
    }
}

NSColor *TintColor(const PaneVisualKind _kind)
{
    switch( _kind ) {
        case PaneVisualKind::PermissionBlocked:
        case PaneVisualKind::PathNotFound:
        case PaneVisualKind::VolumeDisconnected:
        case PaneVisualKind::RemoteUnavailable:
        case PaneVisualKind::Unsupported:
            return NSColor.systemOrangeColor;
        case PaneVisualKind::Error:
            return NSColor.systemRedColor;
        case PaneVisualKind::Unavailable:
        case PaneVisualKind::Loading:
        case PaneVisualKind::Refreshing:
        case PaneVisualKind::Ready:
        case PaneVisualKind::EmptyFolder:
            return NSColor.secondaryLabelColor;
    }
}

bool ShouldPresent(const PaneVisualState &_state) noexcept
{
    if( _state.kind == PaneVisualKind::Unavailable || _state.kind == PaneVisualKind::Ready ||
        _state.kind == PaneVisualKind::Refreshing )
        return false;
    if( IsErrorKind(_state.kind) && _state.content_visible )
        return false;
    return true;
}

} // namespace

/** A deliberately non-semantic row: VoiceOver announces the single loading state instead. */
@interface NCExplorerSkeletonRowView : NSView
@end

@implementation NCExplorerSkeletonRowView

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        self.translatesAutoresizingMaskIntoConstraints = false;
        self.accessibilityElement = false;
    }
    return self;
}

- (void)drawRect:(NSRect)_dirty_rect
{
    [super drawRect:_dirty_rect];
    const NSRect bounds = self.bounds;
    NSColor *const fill = [NSColor.quaternaryLabelColor colorWithAlphaComponent:0.42];
    [fill setFill];

    const auto draw_round = [](const NSRect _rect) {
        [[NSBezierPath bezierPathWithRoundedRect:_rect xRadius:4 yRadius:4] fill];
    };
    draw_round(NSMakeRect(4, 7, 22, 22));
    draw_round(NSMakeRect(38, 18, std::max<CGFloat>(72, bounds.size.width * 0.38), 8));
    draw_round(NSMakeRect(38, 5, std::max<CGFloat>(48, bounds.size.width * 0.22), 7));
}

@end

@implementation NCExplorerPaneStateView {
    PaneVisualKind m_RenderedKind;
    NSStackView *m_Skeleton;
    NSStackView *m_MessageContainer;
    NSProgressIndicator *m_LoadingIndicator;
    NSImageView *m_Icon;
    NSTextField *m_Message;
}

- (BOOL)isOpaque
{
    return true;
}

- (void)drawRect:(NSRect)_dirty_rect
{
    [NSColor.controlBackgroundColor setFill];
    NSRectFill(_dirty_rect);
}

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        m_RenderedKind = PaneVisualKind::Unavailable;
        self.translatesAutoresizingMaskIntoConstraints = false;
        self.hidden = true;
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityIdentifier = @"wincommander.explorer.paneState";
        self.accessibilityLabel = NSLocalizedString(@"Folder state", "Explorer pane-state accessibility label");

        [self buildSkeleton];
        [self buildMessage];
        [self installConstraints];
    }
    return self;
}

- (void)buildSkeleton
{
    m_Skeleton = [[NSStackView alloc] initWithFrame:NSZeroRect];
    m_Skeleton.translatesAutoresizingMaskIntoConstraints = false;
    m_Skeleton.orientation = NSUserInterfaceLayoutOrientationVertical;
    m_Skeleton.alignment = NSLayoutAttributeLeading;
    m_Skeleton.spacing = 5;
    m_Skeleton.accessibilityElement = false;

    for( int index = 0; index != 7; ++index ) {
        NCExplorerSkeletonRowView *const row = [[NCExplorerSkeletonRowView alloc] initWithFrame:NSZeroRect];
        [m_Skeleton addArrangedSubview:row];
        [row.heightAnchor constraintEqualToConstant:36].active = true;
        [row.widthAnchor constraintEqualToAnchor:m_Skeleton.widthAnchor].active = true;
    }
    [self addSubview:m_Skeleton];
}

- (void)buildMessage
{
    m_LoadingIndicator = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    m_LoadingIndicator.translatesAutoresizingMaskIntoConstraints = false;
    m_LoadingIndicator.style = NSProgressIndicatorStyleSpinning;
    m_LoadingIndicator.controlSize = NSControlSizeRegular;
    m_LoadingIndicator.displayedWhenStopped = false;
    m_LoadingIndicator.accessibilityElement = false;

    m_Icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    m_Icon.translatesAutoresizingMaskIntoConstraints = false;
    m_Icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    m_Icon.accessibilityElement = false;
    [m_Icon.widthAnchor constraintEqualToConstant:32].active = true;
    [m_Icon.heightAnchor constraintEqualToConstant:32].active = true;

    m_Message = [NSTextField labelWithString:@""];
    m_Message.translatesAutoresizingMaskIntoConstraints = false;
    m_Message.alignment = NSTextAlignmentCenter;
    m_Message.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    m_Message.textColor = NSColor.secondaryLabelColor;
    m_Message.lineBreakMode = NSLineBreakByWordWrapping;
    m_Message.maximumNumberOfLines = 3;
    m_Message.accessibilityIdentifier = @"wincommander.explorer.paneState.message";
    m_Message.accessibilityLabel = NSLocalizedString(@"Folder status", "Explorer pane-state message accessibility label");

    m_MessageContainer = [[NSStackView alloc] initWithFrame:NSZeroRect];
    m_MessageContainer.translatesAutoresizingMaskIntoConstraints = false;
    m_MessageContainer.orientation = NSUserInterfaceLayoutOrientationVertical;
    m_MessageContainer.alignment = NSLayoutAttributeCenterX;
    m_MessageContainer.spacing = 10;
    [m_MessageContainer addArrangedSubview:m_LoadingIndicator];
    [m_MessageContainer addArrangedSubview:m_Icon];
    [m_MessageContainer addArrangedSubview:m_Message];
    [self addSubview:m_MessageContainer];
}

- (void)installConstraints
{
    [NSLayoutConstraint activateConstraints:@[
        [m_Skeleton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:20],
        [m_Skeleton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-20],
        [m_Skeleton.topAnchor constraintEqualToAnchor:self.topAnchor constant:20],
        [m_MessageContainer.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [m_MessageContainer.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_Message.widthAnchor constraintLessThanOrEqualToAnchor:self.widthAnchor multiplier:0.72],
    ]];
}

- (void)updateWithVisualState:(const PaneVisualState &)_state
{
    m_RenderedKind = _state.kind;
    const bool presenting = ShouldPresent(_state);
    self.hidden = !presenting;

    [m_LoadingIndicator stopAnimation:nil];
    m_LoadingIndicator.hidden = true;
    m_Icon.hidden = true;
    m_Skeleton.hidden = true;
    m_MessageContainer.hidden = !presenting;

    NSString *const message = Message(_state);
    m_Message.stringValue = message;
    m_Message.accessibilityValue = message;
    m_Icon.toolTip = message;
    self.accessibilityValue = message;

    if( !presenting )
        return;

    if( _state.kind == PaneVisualKind::Loading ) {
        m_Skeleton.hidden = false;
        m_LoadingIndicator.hidden = false;
        [m_LoadingIndicator startAnimation:nil];
    }
    else {
        NSString *const symbol = SymbolName(_state.kind);
        if( symbol.length > 0 ) {
            m_Icon.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
            if( m_Icon.image == nil )
                m_Icon.image = [NSImage imageNamed:NSImageNameCaution];
            m_Icon.contentTintColor = TintColor(_state.kind);
            m_Icon.hidden = false;
        }
    }

    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

// Intentionally private observability for focused AppKit tests.
- (PaneVisualKind)renderedKindForTesting
{
    return m_RenderedKind;
}

- (BOOL)skeletonVisibleForTesting
{
    return !m_Skeleton.hidden;
}

- (BOOL)loadingIndicatorVisibleForTesting
{
    return !m_LoadingIndicator.hidden;
}

- (BOOL)iconVisibleForTesting
{
    return !m_Icon.hidden;
}

- (NSString *)messageTextForTesting
{
    return m_Message.stringValue;
}

- (NSColor *)backgroundColorForTesting
{
    return NSColor.controlBackgroundColor;
}

@end
