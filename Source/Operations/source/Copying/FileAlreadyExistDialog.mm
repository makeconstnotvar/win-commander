// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include <sys/stat.h>
#include <Carbon/Carbon.h>
#include "FileAlreadyExistDialog.h"
#include "../ModalDialogResponses.h"
#include "../Internal.h"
#include <Utility/StringExtras.h>
#include <Utility/SheetWithHotkeys.h>
#include <Utility/ObjCpp.h>

@interface NCOpsFileAlreadyExistWindow : NCSheetWithHotkeys
@end

@implementation NCOpsFileAlreadyExistWindow

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    if( event.type == NSEventTypeKeyDown && (event.modifierFlags & NSEventModifierFlagShift) &&
        event.keyCode == kVK_Return ) { // mimic Shift+Enter as enter so hotkey can trigger
        return [super performKeyEquivalent:[NSEvent keyEventWithType:NSEventTypeKeyDown
                                                                  location:event.locationInWindow
                                                             modifierFlags:0
                                                                 timestamp:event.timestamp
                                                              windowNumber:event.windowNumber
                                                                   context:nil
                                                                characters:@"\r"
                                               charactersIgnoringModifiers:@"\r"
                                                                 isARepeat:false
                                                                   keyCode:kVK_Return]];
    }
    return [super performKeyEquivalent:event];
}

@end

@interface NCOpsFileAlreadyExistDialog ()

@property(strong, nonatomic) IBOutlet NSTextField *TargetFilename;
@property(strong, nonatomic) IBOutlet NSTextField *conflictTitle;
@property(strong, nonatomic) IBOutlet NSTextField *sourceItemLabel;
@property(strong, nonatomic) IBOutlet NSTextField *existingItemLabel;
@property(strong, nonatomic) IBOutlet NSTextField *NewFileSize;
@property(strong, nonatomic) IBOutlet NSTextField *ExistingFileSize;
@property(strong, nonatomic) IBOutlet NSTextField *NewFileTime;
@property(strong, nonatomic) IBOutlet NSTextField *ExistingFileTime;
@property(strong, nonatomic) IBOutlet NSButton *RememberCheck;
@property(strong, nonatomic) IBOutlet NSButton *overwriteButton;
@property(strong, nonatomic) IBOutlet NSButton *skipButton;
@property(strong, nonatomic) IBOutlet NSButton *keepBothButton;
@property(strong, nonatomic) IBOutlet NSButton *appendButton;
@property(strong, nonatomic) IBOutlet NSButton *abortButton;
@property(strong, nonatomic) IBOutlet NSPopUpButton *replaceOptionsButton;

- (void)updateApplyToAllAvailability;

@end

@implementation NCOpsFileAlreadyExistDialog {
    std::string m_DestPath;
    struct stat m_SourceStat;
    struct stat m_DestinationStat;
    std::shared_ptr<nc::ops::AsyncDialogResponse> m_Ctx;
    bool m_SingleItem;
}
@synthesize allowAppending;
@synthesize allowKeepingBoth;
@synthesize singleItem = m_SingleItem;
@synthesize TargetFilename;
@synthesize conflictTitle;
@synthesize sourceItemLabel;
@synthesize existingItemLabel;
@synthesize NewFileSize;
@synthesize ExistingFileSize;
@synthesize NewFileTime;
@synthesize ExistingFileTime;
@synthesize RememberCheck;
@synthesize overwriteButton;
@synthesize skipButton;
@synthesize keepBothButton;
@synthesize appendButton;
@synthesize abortButton;
@synthesize replaceOptionsButton;

- (id)initWithDestPath:(const std::string &)_path
         withSourceStat:(const struct stat &)_src_stat
    withDestinationStat:(const struct stat &)_dst_stat
             andContext:(std::shared_ptr<nc::ops::AsyncDialogResponse>)_ctx
{
    using namespace nc::ops;
    const auto nib_path = [Bundle() pathForResource:@"FileAlreadyExistDialog" ofType:@"nib"];
    self = [super initWithWindowNibPath:nib_path owner:self];
    if( self ) {
        m_DestPath = _path;
        m_SourceStat = _src_stat;
        m_DestinationStat = _dst_stat;
        m_Ctx = _ctx;
        self.allowAppending = true;
        self.allowKeepingBoth = false;
        m_SingleItem = false;
    }
    return self;
}

- (void)windowDidLoad
{
    [super windowDidLoad];

    self.TargetFilename.stringValue = [NSString stringWithUTF8StdString:m_DestPath];

    const auto formatter = [[NSDateFormatter alloc] init];
    formatter.timeStyle = NSDateFormatterMediumStyle;
    formatter.dateStyle = NSDateFormatterMediumStyle;

    const auto old_date = [NSDate dateWithTimeIntervalSince1970:static_cast<double>(m_SourceStat.st_mtime)];
    self.NewFileTime.stringValue = [formatter stringFromDate:old_date];
    const auto new_date = [NSDate dateWithTimeIntervalSince1970:static_cast<double>(m_DestinationStat.st_mtime)];
    self.ExistingFileTime.stringValue = [formatter stringFromDate:new_date];

    self.NewFileSize.integerValue = m_SourceStat.st_size;
    self.ExistingFileSize.integerValue = m_DestinationStat.st_size;
    self.RememberCheck.state = NSControlStateValueOff;

    self.window.accessibilityIdentifier = @"wincommander.operation.conflict.window";
    self.window.accessibilityLabel = self.conflictTitle.stringValue;
    self.conflictTitle.accessibilityIdentifier = @"wincommander.operation.conflict.title";
    self.TargetFilename.accessibilityIdentifier = @"wincommander.operation.conflict.destinationPath";
    self.TargetFilename.accessibilityLabel = self.existingItemLabel.stringValue;
    self.NewFileSize.accessibilityIdentifier = @"wincommander.operation.conflict.sourceSize";
    self.NewFileSize.accessibilityLabel = self.sourceItemLabel.stringValue;
    self.NewFileTime.accessibilityIdentifier = @"wincommander.operation.conflict.sourceModified";
    self.NewFileTime.accessibilityLabel = self.sourceItemLabel.stringValue;
    self.ExistingFileSize.accessibilityIdentifier = @"wincommander.operation.conflict.destinationSize";
    self.ExistingFileSize.accessibilityLabel = self.existingItemLabel.stringValue;
    self.ExistingFileTime.accessibilityIdentifier = @"wincommander.operation.conflict.destinationModified";
    self.ExistingFileTime.accessibilityLabel = self.existingItemLabel.stringValue;
    self.RememberCheck.accessibilityIdentifier = @"wincommander.operation.conflict.applyToAll";
    self.RememberCheck.accessibilityLabel = self.RememberCheck.title;
    self.abortButton.accessibilityIdentifier = @"wincommander.operation.conflict.cancel";
    self.abortButton.accessibilityLabel = self.abortButton.title;
    self.appendButton.accessibilityIdentifier = @"wincommander.operation.conflict.append";
    self.appendButton.accessibilityLabel = self.appendButton.title;
    self.keepBothButton.accessibilityIdentifier = @"wincommander.operation.conflict.keepBoth";
    self.keepBothButton.accessibilityLabel = self.keepBothButton.title;
    self.skipButton.accessibilityIdentifier = @"wincommander.operation.conflict.skip";
    self.skipButton.accessibilityLabel = self.skipButton.title;
    self.overwriteButton.accessibilityIdentifier = @"wincommander.operation.conflict.replace";
    self.overwriteButton.accessibilityLabel = self.overwriteButton.title;
    self.replaceOptionsButton.accessibilityIdentifier = @"wincommander.operation.conflict.replaceOptions";
    self.replaceOptionsButton.accessibilityLabel = self.replaceOptionsButton.lastItem.title;
    if( @available(macOS 11.0, *) )
        self.overwriteButton.hasDestructiveAction = true;

    [self updateApplyToAllAvailability];

    NCSheetWithHotkeys *sheet = nc::objc_cast<NCSheetWithHotkeys>(self.window);
    if( !self.singleItem )
        sheet.onCtrlA = [sheet makeClickHotkey:self.RememberCheck];
    sheet.onCtrlK = [sheet makeClickHotkey:self.keepBothButton];
    sheet.onCtrlO = [sheet makeClickHotkey:self.overwriteButton];
    sheet.onCtrlP = [sheet makeClickHotkey:self.appendButton];
    sheet.onCtrlS = [sheet makeClickHotkey:self.skipButton];
}

- (void)setSingleItem:(bool)_singleItem
{
    if( m_SingleItem == _singleItem )
        return;

    m_SingleItem = _singleItem;
    if( self.isWindowLoaded ) {
        [self updateApplyToAllAvailability];
        auto sheet = nc::objc_cast<NCSheetWithHotkeys>(self.window);
        if( m_SingleItem )
            sheet.onCtrlA = nil;
        else
            sheet.onCtrlA = [sheet makeClickHotkey:self.RememberCheck];
    }
}

- (void)updateApplyToAllAvailability
{
    self.RememberCheck.hidden = self.singleItem;
    self.RememberCheck.enabled = !self.singleItem;
    if( self.singleItem )
        self.RememberCheck.state = NSControlStateValueOff;
}

- (IBAction)OnOverwrite:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseOverwrite];
}

- (IBAction)OnOverwriteOlder:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseOverwriteOld];
}

- (IBAction)OnSkip:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseSkip];
}

- (IBAction)OnAppend:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseAppend];
}

- (IBAction)OnCancel:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseStop];
}

- (IBAction)OnKeepBoth:(id) [[maybe_unused]] _sender
{
    [self endDialogWithReturnCode:nc::ops::NSModalResponseKeepBoth];
}

- (void)endDialogWithReturnCode:(NSInteger)_returnCode
{
    if( !self.singleItem && (NSEvent.modifierFlags & NSEventModifierFlagShift) != 0 )
        self.RememberCheck.state = NSControlStateValueOn;

    if( m_Ctx )
        m_Ctx->SetApplyToAll(!self.singleItem && self.RememberCheck.enabled && !self.RememberCheck.hidden &&
                            self.RememberCheck.state == NSControlStateValueOn);

    [self.window.sheetParent endSheet:self.window returnCode:_returnCode];
}

- (void)moveRight:(id)sender
{
    [self.window selectNextKeyView:sender];
}

- (void)moveLeft:(id)sender
{
    [self.window selectPreviousKeyView:sender];
}

@end
