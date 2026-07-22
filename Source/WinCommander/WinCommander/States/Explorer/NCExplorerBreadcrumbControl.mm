// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerBreadcrumbControl.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelController+DataAccess.h"
#include "../../Bootstrap/NativeVFSHostInstance.h"
#include <Panel/PanelData.h>
#include <VFS/VFS.h>
#include <Utility/PathManip.h>

@implementation NCExplorerBreadcrumbControl {
    PanelController *m_Panel;
    NSPathControl *m_PathControl;
    NSTextField *m_FallbackLabel;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        m_PathControl = [[NSPathControl alloc] initWithFrame:self.bounds];
        m_PathControl.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        m_PathControl.pathStyle = NSPathStyleStandard;
        m_PathControl.editable = false; // navigation only - not a drag&drop target or a "choose..." picker
        m_PathControl.backgroundColor = NSColor.clearColor;
        m_PathControl.target = self;
        m_PathControl.action = @selector(onPathControlClicked:);
        [self addSubview:m_PathControl];

        m_FallbackLabel = [[NSTextField alloc] initWithFrame:self.bounds];
        m_FallbackLabel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        m_FallbackLabel.editable = false;
        m_FallbackLabel.selectable = false;
        m_FallbackLabel.bezeled = false;
        m_FallbackLabel.drawsBackground = false;
        m_FallbackLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
        m_FallbackLabel.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
        [self addSubview:m_FallbackLabel];

        [self panelPathChanged];
    }
    return self;
}

- (void)panelPathChanged
{
    if( m_Panel.isUniform && m_Panel.vfs->IsNativeFS() ) {
        const std::string path = m_Panel.currentDirectoryPath;
        NSString *const ns_path = [NSString stringWithUTF8String:path.c_str()];
        m_PathControl.URL = [NSURL fileURLWithPath:ns_path isDirectory:true];
        m_PathControl.hidden = false;
        m_FallbackLabel.hidden = true;
    }
    else {
        m_PathControl.hidden = true;
        m_FallbackLabel.hidden = false;
        m_FallbackLabel.stringValue = [self fallbackDescription];
    }
}

- (NSString *)fallbackDescription
{
    if( m_Panel.isUniform ) {
        // A uniform listing on a non-native VFS - just show the raw path for now, no interactive
        // breadcrumb support for non-native VFS locations in this pass.
        const std::string path = m_Panel.currentDirectoryPath;
        return [NSString stringWithUTF8String:path.c_str()];
    }
    else {
        // A non-uniform listing (e.g. search results, a combined listing) - show its title if any.
        const std::string &title = m_Panel.data.Listing().Title();
        if( !title.empty() )
            return [NSString stringWithUTF8String:title.c_str()];
        return NSLocalizedString(@"Multiple Locations", "Explorer breadcrumb fallback for non-uniform listings");
    }
}

- (void)onPathControlClicked:(id) [[maybe_unused]] _sender
{
    NSPathControlItem *const clicked_item = m_PathControl.clickedPathItem;
    if( clicked_item == nil || clicked_item.URL == nil || !clicked_item.URL.fileURL )
        return;

    const std::string directory = EnsureTrailingSlash(std::string(clicked_item.URL.fileSystemRepresentation));

    auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
    request->RequestedDirectory = directory;
    request->VFS = nc::bootstrap::NativeVFSHostInstance().SharedPtr();
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    [m_Panel GoToDirWithContext:request];
}

@end
