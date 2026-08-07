// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerOperationProgressView.h"

#include <Utility/StringExtras.h>
#include <algorithm>
#include <climits>
#include <cmath>

using nc::core::ExplorerOperationLifecycle;
using nc::core::ExplorerOperationProgressSnapshot;
using nc::core::ExplorerOperationProgressUnit;

namespace {

constexpr CGFloat g_VisibleHeight = 44.0;

NSString *LocalizedValue(NSString *_key, NSString *_fallback)
{
    return [NSBundle.mainBundle localizedStringForKey:_key value:_fallback table:nil];
}

NSString *StateTitle(const ExplorerOperationLifecycle _state)
{
    using enum ExplorerOperationLifecycle;
    switch( _state ) {
        case Queued:
            return LocalizedValue(@"explorer.operations.state.queued", @"Queued");
        case Running:
            return LocalizedValue(@"explorer.operations.state.running", @"Running");
        case Paused:
            return LocalizedValue(@"explorer.operations.state.paused", @"Paused");
        case WaitingForUser:
            return LocalizedValue(@"explorer.operationProgress.state.waiting", @"Waiting for a decision");
        case Finalizing:
            return LocalizedValue(@"explorer.operations.state.finalizing", @"Finalizing");
    }
    return @"";
}

NSString *CountTitle(const size_t _additional_count)
{
    if( _additional_count == 0 )
        return @"";
    return [NSString stringWithFormat:LocalizedValue(@"explorer.operationProgress.additional", @"+%zu more"),
                                      _additional_count];
}

NSString *ProgressTitle(const ExplorerOperationProgressSnapshot &_snapshot)
{
    if( _snapshot.unit == ExplorerOperationProgressUnit::Bytes ) {
        NSByteCountFormatter *const formatter = [NSByteCountFormatter new];
        formatter.countStyle = NSByteCountFormatterCountStyleFile;
        NSString *const processed = [formatter stringFromByteCount:static_cast<long long>(std::min<uint64_t>(
                                                                   _snapshot.processed, LLONG_MAX))];
        if( _snapshot.total == 0 )
            return processed;
        NSString *const total = [formatter stringFromByteCount:static_cast<long long>(
                                                               std::min<uint64_t>(_snapshot.total, LLONG_MAX))];
        return [NSString stringWithFormat:@"%@ / %@", processed, total];
    }

    if( _snapshot.total == 0 )
        return [NSString stringWithFormat:LocalizedValue(@"explorer.operationProgress.items.processed", @"%llu items"),
                                          static_cast<unsigned long long>(_snapshot.processed)];
    return [NSString stringWithFormat:LocalizedValue(@"explorer.operationProgress.items.total", @"%llu / %llu items"),
                                      static_cast<unsigned long long>(_snapshot.processed),
                                      static_cast<unsigned long long>(_snapshot.total)];
}

NSString *SpeedTitle(const ExplorerOperationProgressSnapshot &_snapshot)
{
    if( !_snapshot.speed_per_second || *_snapshot.speed_per_second <= 0.0 )
        return @"";
    if( _snapshot.unit == ExplorerOperationProgressUnit::Bytes ) {
        NSByteCountFormatter *const formatter = [NSByteCountFormatter new];
        formatter.countStyle = NSByteCountFormatterCountStyleFile;
        const auto bytes = static_cast<long long>(
            std::min(*_snapshot.speed_per_second, static_cast<double>(LLONG_MAX)));
        return [[formatter stringFromByteCount:bytes] stringByAppendingString:@"/s"];
    }
    return [NSString stringWithFormat:LocalizedValue(@"explorer.operationProgress.items.speed", @"%.1f items/s"),
                                      *_snapshot.speed_per_second];
}

NSString *ETATitle(const ExplorerOperationProgressSnapshot &_snapshot)
{
    if( !_snapshot.eta )
        return @"";
    const long long seconds = std::max<long long>(0, _snapshot.eta->count());
    NSString *remaining = nil;
    if( seconds >= 3600 )
        remaining = [NSString stringWithFormat:@"%lld:%02lld:%02lld", seconds / 3600, (seconds / 60) % 60, seconds % 60];
    else
        remaining = [NSString stringWithFormat:@"%lld:%02lld", seconds / 60, seconds % 60];
    return [NSString stringWithFormat:LocalizedValue(@"explorer.operationProgress.eta", @"%@ remaining"), remaining];
}

} // namespace

@implementation NCExplorerOperationProgressView {
    NSTextField *m_Title;
    NSTextField *m_CurrentItem;
    NSTextField *m_State;
    NSTextField *m_ProgressText;
    NSTextField *m_Speed;
    NSTextField *m_ETA;
    NSTextField *m_Additional;
    NSProgressIndicator *m_Progress;
    NSLayoutConstraint *m_Height;
    std::optional<ExplorerOperationProgressSnapshot> m_Snapshot;
}

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        self.translatesAutoresizingMaskIntoConstraints = false;
        self.hidden = true;
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityIdentifier = @"wincommander.explorer.operationProgress";
        self.accessibilityLabel = LocalizedValue(@"explorer.operationProgress.accessibility.label", @"File operation progress");
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

    m_Title = make_label(@"wincommander.explorer.operationProgress.title", NSColor.labelColor);
    m_Title.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightMedium];
    m_CurrentItem = make_label(@"wincommander.explorer.operationProgress.currentItem", NSColor.secondaryLabelColor);
    m_CurrentItem.accessibilityLabel =
        LocalizedValue(@"explorer.operationProgress.currentItem.accessibility.label", @"Current file");
    m_State = make_label(@"wincommander.explorer.operationProgress.state", NSColor.secondaryLabelColor);
    m_ProgressText = make_label(@"wincommander.explorer.operationProgress.value", NSColor.secondaryLabelColor);
    m_Speed = make_label(@"wincommander.explorer.operationProgress.speed", NSColor.secondaryLabelColor);
    m_ETA = make_label(@"wincommander.explorer.operationProgress.eta", NSColor.secondaryLabelColor);
    m_Additional = make_label(@"wincommander.explorer.operationProgress.additional", NSColor.tertiaryLabelColor);

    m_Progress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    m_Progress.translatesAutoresizingMaskIntoConstraints = false;
    m_Progress.style = NSProgressIndicatorStyleBar;
    m_Progress.minValue = 0.0;
    m_Progress.maxValue = 1.0;
    m_Progress.displayedWhenStopped = true;
    m_Progress.accessibilityIdentifier = @"wincommander.explorer.operationProgress.indicator";

    NSStackView *const leading = [NSStackView stackViewWithViews:@[m_Title, m_CurrentItem, m_State, m_Additional]];
    leading.translatesAutoresizingMaskIntoConstraints = false;
    leading.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    leading.alignment = NSLayoutAttributeCenterY;
    leading.spacing = 7.0;

    NSStackView *const trailing = [NSStackView stackViewWithViews:@[m_ProgressText, m_Speed, m_ETA]];
    trailing.translatesAutoresizingMaskIntoConstraints = false;
    trailing.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    trailing.alignment = NSLayoutAttributeCenterY;
    trailing.spacing = 9.0;

    NSStackView *const top = [NSStackView stackViewWithViews:@[leading, trailing]];
    top.translatesAutoresizingMaskIntoConstraints = false;
    top.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    top.alignment = NSLayoutAttributeCenterY;
    top.distribution = NSStackViewDistributionFill;
    top.spacing = 12.0;

    NSStackView *const content = [NSStackView stackViewWithViews:@[top, m_Progress]];
    content.translatesAutoresizingMaskIntoConstraints = false;
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.spacing = 4.0;
    [self addSubview:content];

    m_Height = [self.heightAnchor constraintEqualToConstant:0.0];
    m_Height.active = true;
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:10.0],
        [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-10.0],
        [content.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [m_Progress.widthAnchor constraintEqualToAnchor:content.widthAnchor],
        [m_Progress.heightAnchor constraintEqualToConstant:6.0],
        [trailing.trailingAnchor constraintEqualToAnchor:top.trailingAnchor],
    ]];
}

- (void)drawRect:(NSRect)_dirty_rect
{
    [NSColor.controlBackgroundColor setFill];
    NSRectFill(_dirty_rect);
    [NSColor.separatorColor setFill];
    NSRectFill(NSMakeRect(NSMinX(self.bounds), NSMinY(self.bounds), NSWidth(self.bounds), 1.0));
}

- (void)applySnapshot:(const std::optional<ExplorerOperationProgressSnapshot> &)_snapshot
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

    const ExplorerOperationProgressSnapshot &snapshot = *_snapshot;
    self.hidden = false;
    m_Height.constant = g_VisibleHeight;
    m_Title.stringValue = [NSString stringWithUTF8StdString:snapshot.title];
    NSString *const current_item = snapshot.current_item_path
                                       ? [NSString stringWithUTF8StdString:*snapshot.current_item_path]
                                       : @"";
    m_CurrentItem.stringValue = current_item.lastPathComponent.length > 0 ? current_item.lastPathComponent
                                                                          : current_item;
    m_CurrentItem.toolTip = current_item;
    m_State.stringValue = StateTitle(snapshot.lifecycle);
    m_ProgressText.stringValue = ProgressTitle(snapshot);
    m_Speed.stringValue = SpeedTitle(snapshot);
    m_ETA.stringValue = ETATitle(snapshot);
    m_Additional.stringValue = CountTitle(snapshot.additional_count);
    m_State.hidden = m_State.stringValue.length == 0;
    m_CurrentItem.hidden = m_CurrentItem.stringValue.length == 0;
    m_Speed.hidden = m_Speed.stringValue.length == 0;
    m_ETA.hidden = m_ETA.stringValue.length == 0;
    m_Additional.hidden = m_Additional.stringValue.length == 0;

    m_Progress.indeterminate = snapshot.indeterminate;
    if( snapshot.indeterminate )
        [m_Progress startAnimation:nil];
    else {
        [m_Progress stopAnimation:nil];
        m_Progress.doubleValue = std::clamp(snapshot.fraction, 0.0, 1.0);
    }

    NSArray<NSString *> *const accessibility_parts = @[
        m_Title.stringValue,
        current_item,
        m_State.stringValue,
        m_ProgressText.stringValue,
        m_Speed.stringValue,
        m_ETA.stringValue,
        m_Additional.stringValue,
    ];
    NSMutableArray<NSString *> *const nonempty = [NSMutableArray new];
    for( NSString *const part : accessibility_parts )
        if( part.length > 0 )
            [nonempty addObject:part];
    self.accessibilityValue = [nonempty componentsJoinedByString:@", "];
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

// Private observability for focused AppKit tests.
- (CGFloat)visibleHeightForTesting
{
    return m_Height.constant;
}
- (NSString *)titleForTesting
{
    return m_Title.stringValue;
}
- (NSString *)stateForTesting
{
    return m_State.stringValue;
}
- (NSString *)currentItemForTesting
{
    return m_CurrentItem.stringValue;
}
- (NSString *)progressForTesting
{
    return m_ProgressText.stringValue;
}
- (NSString *)speedForTesting
{
    return m_Speed.stringValue;
}
- (NSString *)etaForTesting
{
    return m_ETA.stringValue;
}
- (NSString *)additionalForTesting
{
    return m_Additional.stringValue;
}
- (NSProgressIndicator *)indicatorForTesting
{
    return m_Progress;
}

@end
