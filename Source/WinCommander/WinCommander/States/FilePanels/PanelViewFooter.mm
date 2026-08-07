// Copyright (C) 2016-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelViewFooter.h"
#include <WinCommander/Core/VisualState/VisualStateMapper.h>
#include <Utility/ByteCountFormatter.h>
#include <Utility/ColoredSeparatorLine.h>
#include <Utility/AdaptiveDateFormatting.h>
#include <Utility/StringExtras.h>
#include <Utility/Layout.h>
#include <Base/dispatch_cpp.h>
#include "PanelViewPresentationSettings.h"
#include "PanelViewFooterVolumeInfoFetcher.h"
#include "PanelControllerActionsDispatcher.h"

using namespace nc::panel;
using nc::utility::AdaptiveDateFormatting;

namespace {

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

} // namespace

static NSString *FileSizeToString(const VFSListingItem &_dirent,
                                  const data::ItemVolatileData &_vd,
                                  ByteCountFormatter::Type _format,
                                  ByteCountFormatter &_fmter)
{
    if( _dirent.IsDir() ) {
        if( _vd.is_size_calculated() ) {
            return _fmter.ToNSString(_vd.size, _format);
        }
        else {
            if( _dirent.IsDotDot() ) {
                return NSLocalizedString(@"__MODERNPRESENTATION_UP_WORD",
                                         "Upper-level in directory, for English is 'Up'");
            }
            else {
                return NSLocalizedString(@"__MODERNPRESENTATION_FOLDER_WORD",
                                         "Folders dummy string when size is not available, for English is 'Folder'");
            }
        }
    }
    else {
        return _fmter.ToNSString(_dirent.Size(), _format);
    }
}

static NSString *FormHumanReadableBytesAndFiles(uint64_t _sz,
                                                int _total_files,
                                                ByteCountFormatter::Type _format,
                                                ByteCountFormatter &_fmter)
{
    const auto bytes = _fmter.ToNSString(_sz, _format);
    if( _total_files == 1 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 1 file",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 2 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 2 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 3 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 3 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 4 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 4 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 5 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 5 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 6 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 6 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 7 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 7 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 8 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 8 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else if( _total_files == 9 ) {
        auto fmt =
            NSLocalizedString(@"Selected %@ in 9 files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes];
    }
    else {
        auto fmt =
            NSLocalizedString(@"Selected %@ in %@ files",
                              "Informative text for a bottom information bar in panels, showing size of selection");
        return [NSString stringWithFormat:fmt, bytes, [NSNumber numberWithInt:_total_files]];
    }
}

@interface NCPanelViewFooter ()
- (void)updateExplorerAccessibilityValue;
@end

@implementation NCPanelViewFooter {
    NSColor *m_Background;
    ColoredSeparatorLine *m_SeparatorLine;
    ColoredSeparatorLine *m_VSeparatorLine1;
    ColoredSeparatorLine *m_VSeparatorLine2;
    NSTextField *m_FilenameLabel;
    NSTextField *m_SizeLabel;
    NSTextField *m_ModTime;
    NSTextField *m_ItemsLabel;
    NSTextField *m_VolumeLabel;
    NSTextField *m_SelectionLabel;
    NSButton *m_DetailsButton;
    NSButton *m_IconsButton;
    NSButton *m_ContentButton;

    data::Statistics m_Stats;
    FooterVolumeInfoFetcher m_VolumeInfoFetcher;
    std::unique_ptr<nc::panel::FooterTheme> m_Theme;

    bool m_Active;
    bool m_ExplorerAppearance;

    time_t m_ItemMTime; // need to store this to be able to re-format time when date changes
}

- (id)initWithFrame:(NSRect)frameRect theme:(std::unique_ptr<nc::panel::FooterTheme>)_theme
{
    return [self initWithFrame:frameRect theme:std::move(_theme) explorerAppearance:false];
}

- (id)initWithFrame:(NSRect)frameRect
                theme:(std::unique_ptr<nc::panel::FooterTheme>)_theme
    explorerAppearance:(bool)_explorer_appearance
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Active = false;
        m_ExplorerAppearance = _explorer_appearance;
        m_ItemMTime = 0;
        m_Theme = std::move(_theme);

        if( m_ExplorerAppearance ) {
            self.accessibilityElement = true;
            self.accessibilityRole = NSAccessibilityGroupRole;
            self.accessibilityIdentifier = @"wincommander.explorer.status";
            self.accessibilityLabel = NSLocalizedString(@"Folder status", "Explorer status accessibility label");
        }

        [self createControls];
        [self setupPresentation];

        [self addSubview:m_SeparatorLine];
        if( m_ExplorerAppearance ) {
            [self addSubview:m_ItemsLabel];
            [self addSubview:m_SelectionLabel];
            [self addSubview:m_DetailsButton];
            [self addSubview:m_IconsButton];
            [self addSubview:m_ContentButton];
            [self addSubview:m_VolumeLabel];
        }
        else {
            [self addSubview:m_FilenameLabel];
            [self addSubview:m_SizeLabel];
            [self addSubview:m_ModTime];
            [self addSubview:m_SelectionLabel];
            [self addSubview:m_ItemsLabel];
            [self addSubview:m_VolumeLabel];
            [self addSubview:m_VSeparatorLine1];
            [self addSubview:m_VSeparatorLine2];
        }

        [self installConstraints];

        __weak NCPanelViewFooter *weak_self = self;
        m_VolumeInfoFetcher.SetCallback([=](const VFSStatFS &) {
            if( NCPanelViewFooter *const strong_self = weak_self )
                [strong_self updateVolumeInfo];
        });
        m_Theme->ObserveChanges([weak_self] {
            if( auto strong_self = weak_self )
                [strong_self setupPresentation];
        });

        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(dateDidChange:)
                                                   name:NSCalendarDayChangedNotification
                                                 object:nil];
    }

    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)createControls
{
    // NB! Don't use "single line mode" - it doesn't do what you expect.
    // https://stackoverflow.com/questions/36179012/nstextfield-non-system-font-content-clipped-when-usessinglelinemode-is-true

    m_SeparatorLine = [[ColoredSeparatorLine alloc] initWithFrame:NSRect()];
    m_SeparatorLine.translatesAutoresizingMaskIntoConstraints = NO;

    m_FilenameLabel = [[NSTextField alloc] initWithFrame:NSRect()];
    m_FilenameLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_FilenameLabel.stringValue = @"";
    m_FilenameLabel.bordered = false;
    m_FilenameLabel.editable = false;
    m_FilenameLabel.selectable = false;
    m_FilenameLabel.drawsBackground = false;
    m_FilenameLabel.lineBreakMode = NSLineBreakByTruncatingHead;
    m_FilenameLabel.maximumNumberOfLines = 1;
    m_FilenameLabel.alignment = NSTextAlignmentLeft;
    [m_FilenameLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                              forOrientation:NSLayoutConstraintOrientationHorizontal];

    m_SizeLabel = [[NSTextField alloc] initWithFrame:NSRect()];
    m_SizeLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_SizeLabel.stringValue = @"";
    m_SizeLabel.bordered = false;
    m_SizeLabel.editable = false;
    m_SizeLabel.drawsBackground = false;
    m_SizeLabel.lineBreakMode = NSLineBreakByTruncatingHead;
    m_SizeLabel.maximumNumberOfLines = 1;
    m_SizeLabel.alignment = NSTextAlignmentRight;
    [m_SizeLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];

    m_ModTime = [[NSTextField alloc] initWithFrame:NSRect()];
    m_ModTime.translatesAutoresizingMaskIntoConstraints = false;
    m_ModTime.stringValue = @"";
    m_ModTime.bordered = false;
    m_ModTime.editable = false;
    m_ModTime.drawsBackground = false;
    m_ModTime.lineBreakMode = NSLineBreakByTruncatingHead;
    m_ModTime.maximumNumberOfLines = 1;
    m_ModTime.alignment = NSTextAlignmentRight;
    [m_ModTime setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];

    m_SelectionLabel = [[NSTextField alloc] initWithFrame:NSRect()];
    m_SelectionLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_SelectionLabel.stringValue = @"";
    m_SelectionLabel.bordered = false;
    m_SelectionLabel.editable = false;
    m_SelectionLabel.drawsBackground = false;
    m_SelectionLabel.lineBreakMode = NSLineBreakByTruncatingHead;
    m_SelectionLabel.maximumNumberOfLines = 1;
    m_SelectionLabel.alignment = m_ExplorerAppearance ? NSTextAlignmentLeft : NSTextAlignmentCenter;
    [m_SelectionLabel setContentHuggingPriority:NSLayoutPriorityFittingSizeCompression
                                 forOrientation:NSLayoutConstraintOrientationHorizontal];

    m_ItemsLabel = [[NSTextField alloc] initWithFrame:NSRect()];
    m_ItemsLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_ItemsLabel.stringValue = @"";
    m_ItemsLabel.bordered = false;
    m_ItemsLabel.editable = false;
    m_ItemsLabel.drawsBackground = false;
    m_ItemsLabel.lineBreakMode = NSLineBreakByClipping;
    m_ItemsLabel.maximumNumberOfLines = 1;
    m_ItemsLabel.alignment = m_ExplorerAppearance ? NSTextAlignmentLeft : NSTextAlignmentCenter;
    [m_ItemsLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];

    m_VolumeLabel = [[NSTextField alloc] initWithFrame:NSRect()];
    m_VolumeLabel.translatesAutoresizingMaskIntoConstraints = false;
    m_VolumeLabel.stringValue = @"";
    m_VolumeLabel.bordered = false;
    m_VolumeLabel.editable = false;
    m_VolumeLabel.drawsBackground = false;
    m_VolumeLabel.maximumNumberOfLines = 1;
    m_VolumeLabel.alignment = NSTextAlignmentRight;
    m_VolumeLabel.lineBreakMode = NSLineBreakByClipping;
    [m_VolumeLabel setContentCompressionResistancePriority:40 forOrientation:NSLayoutConstraintOrientationHorizontal];

    if( m_ExplorerAppearance ) {
        m_ItemsLabel.accessibilityIdentifier = @"wincommander.explorer.status.items";
        m_ItemsLabel.accessibilityLabel = NSLocalizedString(@"Items", "Explorer status accessibility label");
        m_SelectionLabel.accessibilityIdentifier = @"wincommander.explorer.status.selection";
        m_SelectionLabel.accessibilityLabel = NSLocalizedString(@"Selection", "Explorer status accessibility label");
        m_VolumeLabel.accessibilityIdentifier = @"wincommander.explorer.status.volume";
        m_VolumeLabel.accessibilityLabel = NSLocalizedString(@"Available space", "Explorer status accessibility label");
        m_DetailsButton = [self makeExplorerViewButtonWithSymbol:@"list.bullet"
                                                           label:NSLocalizedString(@"Details", "Explorer status bar view")
                                                          action:@selector(onToggleViewLayout2:)];
        m_IconsButton = [self makeExplorerViewButtonWithSymbol:@"square.grid.2x2"
                                                         label:NSLocalizedString(@"Icons", "Explorer status bar view")
                                                        action:@selector(onToggleViewLayout3:)];
        m_ContentButton = [self makeExplorerViewButtonWithSymbol:@"rectangle.grid.1x2"
                                                           label:NSLocalizedString(@"Content", "Explorer status bar view")
                                                          action:@selector(onToggleViewLayout5:)];
        m_DetailsButton.accessibilityIdentifier = @"wincommander.explorer.status.view.details";
        m_IconsButton.accessibilityIdentifier = @"wincommander.explorer.status.view.icons";
        m_ContentButton.accessibilityIdentifier = @"wincommander.explorer.status.view.content";
    }
    else {
        m_VSeparatorLine1 = [[ColoredSeparatorLine alloc] initWithFrame:NSRect()];
        m_VSeparatorLine1.translatesAutoresizingMaskIntoConstraints = false;
        [m_VSeparatorLine1 setContentCompressionResistancePriority:40
                                                    forOrientation:NSLayoutConstraintOrientationHorizontal];

        m_VSeparatorLine2 = [[ColoredSeparatorLine alloc] initWithFrame:NSRect()];
        m_VSeparatorLine2.translatesAutoresizingMaskIntoConstraints = false;
        [m_VSeparatorLine2 setContentCompressionResistancePriority:40
                                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
}

- (NSButton *)makeExplorerViewButtonWithSymbol:(NSString *)_symbol label:(NSString *)_label action:(SEL)_action
{
    NSImage *const image = [[NSImage imageWithSystemSymbolName:_symbol accessibilityDescription:_label]
        imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:12.0
                                                                                      weight:NSFontWeightRegular]];
    NSButton *const button = [NSButton buttonWithImage:image target:nil action:_action];
    button.translatesAutoresizingMaskIntoConstraints = false;
    button.bordered = false;
    button.bezelStyle = NSBezelStyleInline;
    button.imagePosition = NSImageOnly;
    button.contentTintColor = NSColor.secondaryLabelColor;
    button.toolTip = _label;
    button.accessibilityLabel = _label;
    button.accessibilityHelp = [NSString
        stringWithFormat:NSLocalizedString(@"Switch to the %@ view", "Explorer view button accessibility help"), _label];
    [button setContentHuggingPriority:NSLayoutPriorityRequired
                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    [button setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    return button;
}

- (void)installConstraints
{
    if( m_ExplorerAppearance ) {
        [NSLayoutConstraint activateConstraints:@[
            [m_SeparatorLine.topAnchor constraintEqualToAnchor:self.topAnchor],
            [m_SeparatorLine.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [m_SeparatorLine.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [m_SeparatorLine.heightAnchor constraintEqualToConstant:1.0],

            [m_ItemsLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8.0],
            [m_ItemsLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor constant:0.5],
            [m_SelectionLabel.leadingAnchor constraintEqualToAnchor:m_ItemsLabel.trailingAnchor constant:10.0],
            [m_SelectionLabel.centerYAnchor constraintEqualToAnchor:m_ItemsLabel.centerYAnchor],
            [m_SelectionLabel.trailingAnchor constraintLessThanOrEqualToAnchor:m_DetailsButton.leadingAnchor
                                                                       constant:-8.0],

            [m_DetailsButton.widthAnchor constraintEqualToConstant:24.0],
            [m_DetailsButton.heightAnchor constraintEqualToConstant:20.0],
            [m_DetailsButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor constant:0.5],
            [m_IconsButton.leadingAnchor constraintEqualToAnchor:m_DetailsButton.trailingAnchor constant:1.0],
            [m_IconsButton.widthAnchor constraintEqualToConstant:24.0],
            [m_IconsButton.heightAnchor constraintEqualToConstant:20.0],
            [m_IconsButton.centerYAnchor constraintEqualToAnchor:m_DetailsButton.centerYAnchor],
            [m_ContentButton.leadingAnchor constraintEqualToAnchor:m_IconsButton.trailingAnchor constant:1.0],
            [m_ContentButton.widthAnchor constraintEqualToConstant:24.0],
            [m_ContentButton.heightAnchor constraintEqualToConstant:20.0],
            [m_ContentButton.centerYAnchor constraintEqualToAnchor:m_DetailsButton.centerYAnchor],

            [m_VolumeLabel.leadingAnchor constraintEqualToAnchor:m_ContentButton.trailingAnchor constant:10.0],
            [m_VolumeLabel.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8.0],
            [m_VolumeLabel.centerYAnchor constraintEqualToAnchor:m_ItemsLabel.centerYAnchor],
            [m_VolumeLabel.widthAnchor constraintGreaterThanOrEqualToConstant:100.0],
        ]];
        return;
    }

    const auto views = NSDictionaryOfVariableBindings(m_SeparatorLine,
                                                      m_FilenameLabel,
                                                      m_SizeLabel,
                                                      m_ModTime,
                                                      m_ItemsLabel,
                                                      m_VolumeLabel,
                                                      m_VSeparatorLine1,
                                                      m_VSeparatorLine2);
    const auto metrics = @{@"lm1": @400, @"lm2": @450};
    const auto ac = [&](NSString *_vf) {
        auto constraints = [NSLayoutConstraint constraintsWithVisualFormat:_vf options:0 metrics:metrics views:views];
        [self addConstraints:constraints];
    };
    ac(@"V:|-(0)-[m_SeparatorLine(==1)]");
    ac(@"V:[m_SeparatorLine]-(==0)-[m_VSeparatorLine1]-(0)-|");
    ac(@"V:[m_SeparatorLine]-(==0)-[m_VSeparatorLine2]-(0)-|");
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_FilenameLabel, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_SizeLabel, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_ModTime, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_ItemsLabel, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_VolumeLabel, self)];

    ac(@"|-(0)-[m_SeparatorLine]-(0)-|");
    ac(@"[m_ModTime]-(>=4@500)-|");
    ac(@"|-(7)-[m_FilenameLabel]-(>=4)-[m_SizeLabel]-(4)-[m_ModTime(>=140@500)]-(4@400)-"
        "[m_VSeparatorLine1(==1@300)]-(2@300)-[m_ItemsLabel(>=50@300)]-(4@300)-"
        "[m_VSeparatorLine2(==1@290)]-(2@300)-[m_VolumeLabel(>=120@280)]-(4@300)-|");
    ac(@"|-(>=lm1@400)-[m_VSeparatorLine1]");
    ac(@"|-(>=lm1@400)-[m_ItemsLabel]");
    ac(@"|-(>=lm2@400)-[m_VSeparatorLine2]");
    ac(@"|-(>=lm2@400)-[m_VolumeLabel]");

    const auto add = [&](NSLayoutConstraint *_lc) { [self addConstraint:_lc]; };
    add([m_SelectionLabel.leadingAnchor constraintEqualToAnchor:m_FilenameLabel.leadingAnchor]);
    add([m_SelectionLabel.topAnchor constraintEqualToAnchor:m_FilenameLabel.topAnchor]);
    add([m_SelectionLabel.bottomAnchor constraintEqualToAnchor:m_FilenameLabel.bottomAnchor]);
    add([m_SelectionLabel.trailingAnchor constraintEqualToAnchor:m_ModTime.trailingAnchor]);
}

static NSString *ComposeFooterFileNameForEntry(const VFSListingItem &_dirent)
{
    // output is a direct filename or symlink path in ->filename form
    if( !_dirent.IsSymlink() ) {
        if( _dirent.Listing()->IsUniform() ) // this looks like a hacky solution
            return _dirent.FilenameNS();     // we're on regular panel - just return filename

        // we're on non-uniform panel like temporary, will return full path
        return [NSString stringWithUTF8StdString:_dirent.Path()];
    }
    else if( _dirent.HasSymlink() ) {
        const auto link = [NSString stringWithUTF8StdString:_dirent.Symlink()];
        if( link != nil )
            return [@"->" stringByAppendingString:link];
    }
    return @""; // fallback case
}

- (void)updateFocusedItem:(const VFSListingItem &)_item VD:(data::ItemVolatileData)_vd // may be empty
{
    if( m_ExplorerAppearance )
        return;

    if( _item ) {
        m_FilenameLabel.stringValue = ComposeFooterFileNameForEntry(_item);
        m_FilenameLabel.toolTip = [NSString stringWithUTF8StdString:_item.Path()];
        m_SizeLabel.stringValue = FileSizeToString(_item, _vd, GetFileSizeFormat(), ByteCountFormatter::Instance());
        m_ItemMTime = _item.MTime();
    }
    else {
        m_FilenameLabel.stringValue = @"";
        m_SizeLabel.stringValue = @"";
        m_ItemMTime = 0;
    }
    [self updateModTime];
}

- (void)updateModTime
{
    if( m_ItemMTime > 0 ) {
        const auto style = AdaptiveDateFormatting::Style::Medium;
        m_ModTime.stringValue = AdaptiveDateFormatting::Format(style, m_ItemMTime);
    }
    else {
        m_ModTime.stringValue = @"";
    }
}

- (BOOL)canDrawSubviewsIntoLayer
{
    return true;
}

- (BOOL)isOpaque
{
    return true;
}

- (void)drawRect:(NSRect) [[maybe_unused]] dirtyRect
{
    if( m_Background && m_Background != NSColor.clearColor ) {
        auto context = NSGraphicsContext.currentContext.CGContext;
        CGContextSetFillColorWithColor(context, m_Background.CGColor);
        CGContextFillRect(context, NSRectToCGRect(self.bounds));
    }
    else {
        NSDrawWindowBackground(self.bounds);
    }
}

- (void)setupPresentation
{
    if( m_ExplorerAppearance ) {
        m_Background = NSColor.controlBackgroundColor;
        const auto font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        m_ItemsLabel.font = font;
        m_SelectionLabel.font = font;
        m_VolumeLabel.font = font;
        m_ItemsLabel.textColor = NSColor.labelColor;
        m_SelectionLabel.textColor = NSColor.secondaryLabelColor;
        m_VolumeLabel.textColor = NSColor.secondaryLabelColor;
        m_SeparatorLine.borderColor = NSColor.separatorColor;
        [self setNeedsDisplay:true];
        return;
    }

    const bool active = m_Active;
    m_Background = active ? m_Theme->ActiveBackgroundColor() : m_Theme->InactiveBackgroundColor();

    auto font = m_Theme->Font();
    m_FilenameLabel.font = font;
    m_SizeLabel.font = font;
    m_ModTime.font = font;
    m_ItemsLabel.font = font;
    m_VolumeLabel.font = font;
    m_SelectionLabel.font = font;

    const auto text_color = active ? m_Theme->ActiveTextColor() : m_Theme->TextColor();
    m_FilenameLabel.textColor = text_color;
    m_SizeLabel.textColor = text_color;
    m_ModTime.textColor = text_color;
    m_ItemsLabel.textColor = text_color;
    m_VolumeLabel.textColor = text_color;
    m_SelectionLabel.textColor = text_color;

    auto separator_color = m_Theme->SeparatorsColor();
    m_SeparatorLine.borderColor = separator_color;
    m_VSeparatorLine1.borderColor = separator_color;
    m_VSeparatorLine2.borderColor = separator_color;

    [self setNeedsDisplay:true];
}

- (void)updateStatistics:(const data::Statistics &)_stats
{
    // Explorer accepts counts only from PaneStore. PanelView still calls this legacy path for
    // Commander, so accepting it here would let PanelData overwrite a newer Store snapshot.
    if( m_ExplorerAppearance )
        return;

    if( m_Stats == _stats )
        return;

    m_Stats = _stats;

    m_ItemsLabel.stringValue = [NSString stringWithFormat:@"(%d)", m_Stats.total_entries_amount];

    if( m_Stats.selected_entries_amount == 0 ) {
        m_SelectionLabel.stringValue = @"";
        m_SelectionLabel.hidden = true;
        m_FilenameLabel.hidden = false;
        m_SizeLabel.hidden = false;
        m_ModTime.hidden = false;
    }
    else {
        const auto sel_str = FormHumanReadableBytesAndFiles(m_Stats.bytes_in_selected_entries,
                                                            m_Stats.selected_entries_amount,
                                                            GetSelectionSizeFormat(),
                                                            ByteCountFormatter::Instance());
        m_SelectionLabel.stringValue = sel_str;
        m_SelectionLabel.hidden = false;
        m_FilenameLabel.hidden = true;
        m_SizeLabel.hidden = true;
        m_ModTime.hidden = true;
    }
}

- (void)updateListing:(const VFSListingPtr &)_listing
{
    if( m_ExplorerAppearance )
        return;
    m_VolumeInfoFetcher.SetTarget(_listing);
    [self updateVolumeInfo];
}

- (void)applyExplorerPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
{
    dispatch_assert_main_queue();
    if( !m_ExplorerAppearance )
        return;

    const nc::core::PaneStatusVisualState &status = nc::core::VisualStateMapper::MapPane(_snapshot).status;
    switch( status.kind ) {
        case nc::core::PaneStatusVisualKind::Counts: {
            if( status.item_count < 0 ) {
                m_ItemsLabel.stringValue = @"";
                m_SelectionLabel.stringValue = @"";
                break;
            }

            const auto items_format =
                NSLocalizedString(@"%d items", "Explorer status bar, total number of items in the current directory");
            m_ItemsLabel.stringValue = [NSString stringWithFormat:items_format, status.item_count];

            if( status.selected_count > 0 && status.selected_bytes >= 0 ) {
                const auto size =
                    ByteCountFormatter::Instance().ToNSString(status.selected_bytes, ByteCountFormatter::Adaptive6);
                const auto selection_format = NSLocalizedString(
                    @"%d selected (%@)", "Explorer status bar, number and total size of currently selected items");
                m_SelectionLabel.stringValue = [NSString stringWithFormat:selection_format, status.selected_count, size];
            }
            else {
                m_SelectionLabel.stringValue = @"";
            }
            break;
        }
        case nc::core::PaneStatusVisualKind::Loading:
        case nc::core::PaneStatusVisualKind::Empty:
        case nc::core::PaneStatusVisualKind::Error:
            m_ItemsLabel.stringValue = status.message ? LocalizedVisualMessage(*status.message) : @"";
            m_SelectionLabel.stringValue = @"";
            break;
        case nc::core::PaneStatusVisualKind::Unavailable:
            m_ItemsLabel.stringValue = @"";
            m_SelectionLabel.stringValue = @"";
            break;
    }

    // SetTarget requires a non-null listing. Retaining the previous volume presentation while a
    // new location is loading is deliberate; volume-state ownership is outside this count slice.
    if( _snapshot.state.listing ) {
        m_VolumeInfoFetcher.SetTarget(_snapshot.state.listing);
        [self updateVolumeInfo];
    }
    [self updateExplorerAccessibilityValue];
}

- (void)updateExplorerAccessibilityValue
{
    if( !m_ExplorerAppearance )
        return;
    NSMutableArray<NSString *> *const components = [NSMutableArray arrayWithCapacity:3];
    for( NSTextField *label in @[m_ItemsLabel, m_SelectionLabel, m_VolumeLabel] ) {
        label.accessibilityValue = label.stringValue;
        if( label.stringValue.length > 0 ) {
            [components addObject:label.stringValue];
        }
    }
    self.accessibilityValue = [components componentsJoinedByString:@", "];
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}

- (void)updateVolumeInfo
{
    if( m_ExplorerAppearance ) {
        const auto &fmter = ByteCountFormatter::Instance();
        const auto avail = fmter.ToNSString(m_VolumeInfoFetcher.Current().avail_bytes, ByteCountFormatter::Adaptive6);
        const auto fmt = NSLocalizedString(@"%@ available", "Explorer status bar, free space available on volume");
        m_VolumeLabel.stringValue = [NSString stringWithFormat:fmt, avail];
        m_VolumeLabel.toolTip = [NSString stringWithUTF8StdString:m_VolumeInfoFetcher.Current().volume_name];
        [self updateExplorerAccessibilityValue];
        return;
    }

    const auto fmt = NSLocalizedString(@"%@ available", "Panels bottom volume bar, showing amount of bytes available");
    const auto &fmter = ByteCountFormatter::Instance();
    const auto avail = fmter.ToNSString(m_VolumeInfoFetcher.Current().avail_bytes, ByteCountFormatter::Adaptive6);
    m_VolumeLabel.stringValue = [NSString stringWithFormat:fmt, avail];

    m_VolumeLabel.toolTip = [NSString stringWithUTF8StdString:m_VolumeInfoFetcher.Current().volume_name];
}

- (void)updateExplorerLayoutKind:(NCPanelViewFooterLayoutKind)_layout_kind
{
    if( !m_ExplorerAppearance )
        return;

    const auto update = ^(NSButton *_button, bool _selected) {
      _button.state = _selected ? NSControlStateValueOn : NSControlStateValueOff;
      _button.contentTintColor = _selected ? NSColor.controlAccentColor : NSColor.secondaryLabelColor;
      _button.accessibilityValue = _selected ? NSLocalizedString(@"Selected", "Accessibility value")
                                             : NSLocalizedString(@"Not selected", "Accessibility value");
    };
    update(m_DetailsButton, _layout_kind == NCPanelViewFooterLayoutKindDetails);
    update(m_IconsButton, _layout_kind == NCPanelViewFooterLayoutKindIcons);
    update(m_ContentButton, _layout_kind == NCPanelViewFooterLayoutKindContent);
}

- (void)viewDidMoveToWindow
{
    if( self.window )
        m_VolumeInfoFetcher.ResumeUpdates();
    else
        m_VolumeInfoFetcher.PauseUpdates();
}

- (void)setActive:(bool)active
{
    if( m_Active == active )
        return;

    m_Active = active;
    [self setupPresentation];
}

- (bool)active
{
    return m_Active;
}

- (bool)explorerAppearance
{
    return m_ExplorerAppearance;
}

- (CGFloat)preferredHeight
{
    return m_ExplorerAppearance ? 24.0 : 20.0;
}

- (void)dateDidChange:(NSNotification *) [[maybe_unused]] _notification
{
    // may be triggered from a background notification thread, so kick the handling to the main
    // thread
    __weak NCPanelViewFooter *weak_self = self;
    dispatch_to_main_queue([weak_self] {
        if( NCPanelViewFooter *const strong_self = weak_self )
            [strong_self updateModTime];
    });
}

@end
