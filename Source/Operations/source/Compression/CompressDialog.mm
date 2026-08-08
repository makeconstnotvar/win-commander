// Copyright (C) 2019-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CompressDialog.h"
#include <Operations/Localizable.h>
#include <Utility/StringExtras.h>
#include <Utility/ObjCpp.h>
#include <Operations/FilenameTextControl.h>
#include "../Internal.h"

@interface NCOpsCompressDialog ()
@property(weak, nonatomic) IBOutlet NSButton *compressButton;
@property(weak, nonatomic) IBOutlet NSButton *cancelButton;
@property(weak, nonatomic) IBOutlet NSButton *protectWithPasswordCheckbox;
@property(weak, nonatomic) IBOutlet NSSecureTextField *passwordTextField;
@property(weak, nonatomic) IBOutlet NSTextField *destinationTextField;
@property(weak, nonatomic) IBOutlet NSTextField *destinationTitleTextField;
@property(weak, nonatomic) IBOutlet NSPopUpButton *formatPopUp;
@property(nonatomic) bool protectWithPassword;
@property(nonatomic) bool formatSupportsEncryption;
@property(nonatomic) bool validInput;
@property(nonatomic) NSString *destinationString;
@property(nonatomic) NSString *passwordString;

@end

@implementation NCOpsCompressDialog {
    std::vector<VFSListingItem> m_SourceItems;
    VFSHostPtr m_DestinationHost;
    std::string m_InitialDestination;
    std::string m_FinalDestination;
    std::string m_FinalPassword;
    nc::ops::ArchiveCreationFormat m_SelectedFormat;
    std::shared_ptr<nc::ops::DirectoryPathAutoCompetion> m_AutoCompletion;
    NCFilepathAutoCompletionDelegate *m_AutoCompletionDelegate;
}

@synthesize destination = m_FinalDestination;
@synthesize password = m_FinalPassword;
@synthesize compressButton;
@synthesize cancelButton;
@synthesize protectWithPasswordCheckbox;
@synthesize passwordTextField;
@synthesize destinationTextField;
@synthesize destinationTitleTextField;
@synthesize formatPopUp;
@synthesize format = m_SelectedFormat;
@synthesize protectWithPassword;
@synthesize formatSupportsEncryption;
@synthesize validInput;
@synthesize destinationString;
@synthesize passwordString;

- (instancetype)initWithItems:(const std::vector<VFSListingItem> &)_source_items
               destinationVFS:(const VFSHostPtr &)_destination_host
           initialDestination:(const std::string &)_initial_destination
{
    using namespace nc::ops;
    const auto nib_path = [Bundle() pathForResource:@"CompressDialog" ofType:@"nib"];
    self = [super initWithWindowNibPath:nib_path owner:self];
    if( self ) {
        m_SourceItems = _source_items;
        m_DestinationHost = _destination_host;
        m_InitialDestination = _initial_destination;
        self.protectWithPassword = false;
        m_SelectedFormat = nc::ops::ArchiveCreationFormat::Zip;
        self.formatSupportsEncryption = true;
        self.destinationString = [NSString stringWithUTF8StdString:m_InitialDestination];
        self.validInput = false;
        m_AutoCompletion = std::make_shared<nc::ops::DirectoryPathAutoCompletionImpl>(m_DestinationHost);
        m_AutoCompletionDelegate = [[NCFilepathAutoCompletionDelegate alloc] init];
        m_AutoCompletionDelegate.completion = m_AutoCompletion;
        m_AutoCompletionDelegate.isNativeVFS = m_DestinationHost->IsNativeFS();
        [self validate];
    }
    return self;
}

- (void)windowDidLoad
{
    using namespace nc::ops;
    [super windowDidLoad];

    // Built from the model rather than listed in the nib, so the menu cannot come to offer a format
    // the engine does not produce - which is the whole reason the creatable set is modelled apart
    // from the extractable one.
    [self.formatPopUp removeAllItems];
    for( const ArchiveCreationFormatInfo &info : SupportedArchiveCreationFormats() ) {
        NSMenuItem *const item = [[NSMenuItem alloc] init];
        item.title = [NSString stringWithUTF8StdStringView:info.extension];
        item.tag = static_cast<NSInteger>(info.format);
        [self.formatPopUp.menu addItem:item];
    }
    [self.formatPopUp selectItemWithTag:static_cast<NSInteger>(m_SelectedFormat)];
    [self applyFormatSelection];
    const auto amount = static_cast<int>(m_SourceItems.size());
    if( amount > 1 )
        self.destinationTitleTextField.stringValue = [NSString
            stringWithFormat:localizable::CompressionDiaglogCompressItemsToTitle(), [NSNumber numberWithInt:amount]];
    else
        self.destinationTitleTextField.stringValue = [NSString
            stringWithFormat:localizable::CompressionDiaglogCompressItemToTitle(), m_SourceItems.front().FilenameNS()];
}

- (void)validate
{
    // The same rule the compression job enforces before it writes anything, asked here so the user
    // is stopped at the dialog rather than told no after pressing a button.
    const nc::ops::ArchiveCreationRequestVerdict verdict =
        nc::ops::EvaluateArchiveCreationRequest(m_SelectedFormat,
                                                self.destinationString.length != 0,
                                                self.protectWithPassword,
                                                self.passwordString.length != 0);
    self.validInput = verdict == nc::ops::ArchiveCreationRequestVerdict::Submittable;
}

- (void)applyFormatSelection
{
    const bool encryptable = nc::ops::DescribeArchiveCreationFormat(m_SelectedFormat).supports_encryption;
    // Bound in the nib rather than assigned to the control here. A control whose `value` is bound has
    // its `enabled` managed by the bindings machinery, so a direct assignment is overwritten the next
    // time the bound value changes - which is exactly what happens on the line below.
    self.formatSupportsEncryption = encryptable;
    // Switching to a format that cannot carry a passphrase clears the request rather than leaving a
    // ticked box that would produce an unprotected archive. What was typed stays in the field, so
    // switching back to a format that can encrypt restores it instead of asking for it again.
    if( !encryptable && self.protectWithPassword )
        self.protectWithPassword = false;
    [self validate];
}

- (IBAction)onFormatChanged:(id) [[maybe_unused]] _sender
{
    m_SelectedFormat = static_cast<nc::ops::ArchiveCreationFormat>(self.formatPopUp.selectedTag);
    [self applyFormatSelection];
}

- (IBAction)onCompress:(id) [[maybe_unused]] _sender
{
    m_FinalDestination = self.destinationString.decomposedStringWithCanonicalMapping.UTF8String;

    if( self.protectWithPassword )
        m_FinalPassword = self.passwordString.UTF8String;

    [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseOK];
}

- (IBAction)onCancel:(id) [[maybe_unused]] _sender
{
    [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseCancel];
}

- (void)controlTextDidChange:(NSNotification *)notification
{
    if( nc::objc_cast<NSTextField>(notification.object) == self.destinationTextField ||
        nc::objc_cast<NSTextField>(notification.object) == self.passwordTextField )
        [self validate];
}

- (IBAction)onProtectWithPassword:(id) [[maybe_unused]] _sender
{
    [self validate];
}

- (BOOL)control:(NSControl *)_control textView:(NSTextView *)_text_view doCommandBySelector:(SEL)_command_selector
{
    if( _control == self.destinationTextField && _command_selector == @selector(complete:) ) {
        return [m_AutoCompletionDelegate control:_control textView:_text_view doCommandBySelector:_command_selector];
    }
    return false;
}

@end
