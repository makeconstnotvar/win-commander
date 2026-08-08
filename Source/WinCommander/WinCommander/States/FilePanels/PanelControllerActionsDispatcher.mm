// Copyright (C) 2018-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelControllerActionsDispatcher.h"
#include <Utility/ActionsShortcutsManager.h>
#include <WinCommander/Core/Alert.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/States/CommandPresentationAdapter.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorPresenting.h>
#include <Panel/PanelData.h>
#include <Utility/NSMenu+Hierarchical.h>
#include "Actions/CopyToPasteboard.h"
#include "PanelController.h"
#include "PanelHistory.h"
#include "PanelView.h"
#include "Helpers/Pasteboard.h"
#include "Actions/GoToFolder.h"
#include "Actions/OpenFile.h"
#include "Actions/Enter.h"
#include <algorithm>
#include <iostream>

using namespace nc::core;
using namespace nc::panel;
namespace nc::panel {

static const actions::PanelAction *ActionBySel(SEL _sel, const PanelActionsMap &_map) noexcept;
static void Perform(SEL _sel, const PanelActionsMap &_map, PanelController *_target, id _sender);
static CommandState FailedFileOpenCommandState();
static CommandState FailedFileCopyCommandState();
static CommandState FailedFileCutCommandState();
static CommandState FailedFileMutationCommandState(std::string_view _command_id);
static CommandState FailedFileRenameCommandState();
static CommandState FailedViewToggleHiddenFilesCommandState();
static CommandState FailedNavigationHistoryCommandState(std::string_view _command_id);
static CommandState FailedPaneNavigationCommandState(std::string_view _command_id);

static id<NCExplorerInspectorPresenting> InspectorPresenter(PanelController *_panel) noexcept
{
    id const state = _panel.state;
    if( ![state conformsToProtocol:@protocol(NCExplorerInspectorPresenting)] )
        return nil;
    return static_cast<id<NCExplorerInspectorPresenting>>(state);
}

} // namespace nc::panel

@implementation NCPanelControllerActionsDispatcher {
    __unsafe_unretained PanelController *m_PC;
    const nc::panel::PanelActionsMap *m_AM;
    const nc::utility::ActionsShortcutsManager *m_ActionsShortcutsManager;
    nc::core::CommandRegistry *m_CommandRegistry;
}

- (CommandInvocationSource)commandInvocationSourceForSender:(id)_sender
                                                   commandId:(const std::string_view)_command_id
                                                currentEvent:(NSEvent *)_event
{
    if( [_sender isKindOfClass:NSMenuItem.class] ) {
        NSMenuItem *const item = static_cast<NSMenuItem *>(_sender);
        if( nc::presentation::CommandPresentationAdapter::IsKeyEquivalentInvocation(item, _event) )
            return CommandInvocationSource::Shortcut;
        if( _event.type == NSEventTypeKeyDown ) {
            const auto *const descriptor = m_CommandRegistry->Find(CommandId{_command_id});
            if( descriptor && descriptor->legacy ) {
                const nc::utility::ActionShortcut current{
                    nc::utility::ActionShortcut::EventData(_event)};
                for( const std::string &action_name : descriptor->legacy->shortcut_action_names ) {
                    if( const auto shortcuts = m_ActionsShortcutsManager->ShortcutsFromAction(action_name) ) {
                        if( std::ranges::find(*shortcuts, current) != shortcuts->end() )
                            return CommandInvocationSource::Shortcut;
                    }
                }
            }
        }
        return CommandInvocationSource::Menu;
    }
    if( [_sender isKindOfClass:NSButton.class] )
        return CommandInvocationSource::Toolbar;
    return CommandInvocationSource::Programmatic;
}

- (CommandInvocationSource)commandInvocationSourceForSender:(id)_sender
                                                   commandId:(const std::string_view)_command_id
{
    return [self commandInvocationSourceForSender:_sender commandId:_command_id currentEvent:NSApp.currentEvent];
}

- (instancetype)initWithController:(PanelController *)_controller
                        actionsMap:(const nc::panel::PanelActionsMap &)_actions_map
           actionsShortcutsManager:(const nc::utility::ActionsShortcutsManager &)_actions_shortcuts_manager
                   commandRegistry:(nc::core::CommandRegistry &)_command_registry
{
    self = [super init];
    if( self ) {
        m_PC = _controller;
        m_AM = &_actions_map;
        m_ActionsShortcutsManager = &_actions_shortcuts_manager;
        m_CommandRegistry = &_command_registry;
    }
    return self;
}

- (int)bidForHandlingKeyDown:(NSEvent *)_event forPanelView:(PanelView *)_panel_view
{
    return [self bidForHandlingKeyDown:_event forPanelView:_panel_view andHandle:false];
}

- (void)handleKeyDown:(NSEvent *)_event forPanelView:(PanelView *)_panel_view
{
    [self bidForHandlingKeyDown:_event forPanelView:_panel_view andHandle:true];
}

- (int)bidForHandlingKeyDown:(NSEvent *)_event
                forPanelView:(PanelView *) [[maybe_unused]] _panel_view
                   andHandle:(bool)_handle
{
    if( !m_PC.isDisplayingCommittedData )
        return view::BiddingPriority::Skip;

    struct Tags {
        int file_enter = -1;
        int file_open = -1;
        int go_root = -1;
        int go_home = -1;
        int show_preview = -1;
        int go_into_folder = -1;
        int go_into_enclosing_folder = -1;
        int show_context_menu = -1;
    };
    const Tags tags = [&] {
        Tags t;
        t.file_enter = m_ActionsShortcutsManager->TagFromAction("menu.file.enter").value();
        t.file_open = m_ActionsShortcutsManager->TagFromAction("menu.file.open").value();
        t.go_root = m_ActionsShortcutsManager->TagFromAction("panel.go_root").value();
        t.go_home = m_ActionsShortcutsManager->TagFromAction("panel.go_home").value();
        t.show_preview = m_ActionsShortcutsManager->TagFromAction("panel.show_preview").value();
        t.go_into_folder = m_ActionsShortcutsManager->TagFromAction("panel.go_into_folder").value();
        t.go_into_enclosing_folder = m_ActionsShortcutsManager->TagFromAction("panel.go_into_enclosing_folder").value();
        t.show_context_menu = m_ActionsShortcutsManager->TagFromAction("panel.show_context_menu").value();
        return t;
    }();

    const std::optional<int> event_action_tag = m_ActionsShortcutsManager->FirstOfActionTagsFromShortcut(
        {reinterpret_cast<const int *>(&tags), sizeof(tags) / sizeof(int)},
        nc::utility::ActionShortcut::EventData(_event));

    if( event_action_tag == tags.show_preview ) {
        if( _handle ) {
            [self executeFilePreviewCommandFromSource:CommandInvocationSource::Shortcut sender:_event];
            return view::BiddingPriority::High;
        }
        else
            return [self validateActionBySelector:@selector(OnFileViewCommand:)] ? view::BiddingPriority::High
                                                                                 : view::BiddingPriority::Skip;
    }

    if( event_action_tag == tags.go_home ) {
        if( _handle ) {
            static int tag = m_ActionsShortcutsManager->TagFromAction("menu.go.home").value();
            [[NSApp menu] performActionForItemWithTagHierarchical:tag];
        }
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.go_root ) {
        if( _handle ) {
            static int tag = m_ActionsShortcutsManager->TagFromAction("menu.go.root").value();
            [[NSApp menu] performActionForItemWithTagHierarchical:tag];
        }
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.go_into_folder ) {
        if( _handle ) {
            static int tag = m_ActionsShortcutsManager->TagFromAction("menu.go.into_folder").value();
            [[NSApp menu] performActionForItemWithTagHierarchical:tag];
        }
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.go_into_enclosing_folder ) {
        if( _handle ) {
            static int tag = m_ActionsShortcutsManager->TagFromAction("menu.go.enclosing_folder").value();
            [[NSApp menu] performActionForItemWithTagHierarchical:tag];
        }
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.file_enter ) {
        if( _handle ) {
            // we keep it here to avoid blinking on menu item
            if( actions::Enter::UsesDefaultFileOpen(m_PC) )
                [self executeFileOpenCommandFromSource:CommandInvocationSource::Shortcut sender:_event];
            else
                [self OnOpen:_event];
        }
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.file_open ) {
        if( _handle )
            [self executeFileOpenCommandFromSource:CommandInvocationSource::Shortcut sender:_event];
        return view::BiddingPriority::High;
    }

    if( event_action_tag == tags.show_context_menu ) {
        if( _handle ) {
            [self executeBySelectorIfValidOrBeep:@selector(onShowContextMenu:) withSender:self];
        }
        return view::BiddingPriority::High;
    }

    return view::BiddingPriority::Skip;
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    try {
        if( !m_PC.isDisplayingCommittedData )
            return false;
        if( item.action == @selector(copy:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileCopy];
            const CommandState state = [self fileCopyCommandStateFromSource:source];
            if( source == CommandInvocationSource::Menu ) {
                actions::UpdateCopyToPasteboardMenuItemTitle(m_PC, item);
                return nc::presentation::CommandPresentationAdapter::Apply(state, item);
            }

            // The Edit Copy item is shared with the responder chain. Keyboard validation must not
            // leave panel-specific presentation on the persistent menu item.
            nc::presentation::CommandPresentationAdapter::Clear(item);
            return state.visible && state.enabled;
        }
        if( item.action == @selector(cut:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileCut];
            const CommandState state = [self fileCutCommandStateFromSource:source];
            if( source == CommandInvocationSource::Menu ) {
                actions::UpdateCutToPasteboardMenuItemTitle(m_PC, item);
                return nc::presentation::CommandPresentationAdapter::Apply(state, item);
            }

            // The Edit Cut item is shared with the responder chain. Preserve field-editor handling
            // and do not leave panel-specific presentation on the persistent menu item.
            nc::presentation::CommandPresentationAdapter::Clear(item);
            return state.visible && state.enabled;
        }
        if( item.action == @selector(paste:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FilePaste];
            const CommandState state = [self filePasteCommandStateFromSource:source];
            if( source == CommandInvocationSource::Menu )
                return nc::presentation::CommandPresentationAdapter::Apply(state, item);

            // Edit Paste is shared with text controls. Clear persistent panel presentation for a
            // key-equivalent validation so the field editor keeps responder precedence.
            nc::presentation::CommandPresentationAdapter::Clear(item);
            return state.visible && state.enabled;
        }
        if( item.action == @selector(OnQuickNewFolder:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileNewFolder];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileNewFolderCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnQuickNewFile:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileNewFile];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileNewFileCommandStateFromSource:source], item);
        }
        if( item.action == @selector(selectAll:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::PaneSelectAll];
            const CommandState state = [self paneSelectAllCommandStateFromSource:source];
            if( source == CommandInvocationSource::Menu )
                return nc::presentation::CommandPresentationAdapter::Apply(state, item);

            // Edit Select All belongs to the focused text control before the pane responder.
            nc::presentation::CommandPresentationAdapter::Clear(item);
            return state.visible && state.enabled;
        }
        if( item.action == @selector(OnMenuInvertSelection:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::PaneInvertSelection];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self paneInvertSelectionCommandStateFromSource:source], item);
        }
        if( item.action == @selector(onCompressItemsHere:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::ArchiveCreate];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self archiveCreateCommandStateFromSource:source], item);
        }
        if( item.action == @selector(onExtractArchiveHere:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::ArchiveExtract];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self archiveExtractCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnDuplicate:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileDuplicate];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileDuplicateCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnCopyCurrentFilePath:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileCopyPath];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileCopyPathCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnCalculateSizes:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileCalculateSizes];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileCalculateSizesCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnBatchRename:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileBatchRename];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileBatchRenameCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnMoveToTrash:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileTrash];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileTrashCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnDeletePermanentlyCommand:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileDelete];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileDeleteCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnRenameFileInPlace:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileRename];
            const CommandState state = [self fileRenameCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnFileGetInfo:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileGetInfo];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self fileGetInfoCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnFileViewCommand:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FilePreview];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self filePreviewCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnOpenNatively:) ) {
            actions::UpdateOpenWithDefaultHandlerMenuItemTitle(m_PC, item);
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::FileOpen];
            const CommandState state = [self fileOpenCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnOpen:) ) {
            if( actions::Enter::UsesDefaultFileOpen(m_PC) ) {
                actions::UpdateOpenWithDefaultHandlerMenuItemTitle(m_PC, item);
                const CommandInvocationSource source =
                    [self commandInvocationSourceForSender:item commandId:command_ids::FileOpen];
                const CommandState state = [self fileOpenCommandStateFromSource:source];
                return nc::presentation::CommandPresentationAdapter::Apply(state, item);
            }
        }
        if( item.action == @selector(ToggleViewHiddenFiles:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::ViewToggleHiddenFiles];
            const CommandState state = [self viewToggleHiddenFilesCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnTogglePreviewPane:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::ViewTogglePreviewPane];
            return nc::presentation::CommandPresentationAdapter::Apply(
                [self viewTogglePreviewPaneCommandStateFromSource:source], item);
        }
        if( item.action == @selector(OnGoBack:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::NavigationBack];
            const CommandState state = [self navigationBackCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnGoForward:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::NavigationForward];
            const CommandState state = [self navigationForwardCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnGoToUpperDirectory:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::NavigationUp];
            const CommandState state = [self navigationUpCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( item.action == @selector(OnRefreshPanel:) ) {
            const CommandInvocationSource source =
                [self commandInvocationSourceForSender:item commandId:command_ids::NavigationRefresh];
            const CommandState state = [self navigationRefreshCommandStateFromSource:source];
            return nc::presentation::CommandPresentationAdapter::Apply(state, item);
        }
        if( const auto action = ActionBySel(item.action, *m_AM) )
            return action->ValidateMenuItem(m_PC, item);
        return true;
    } catch( const std::exception &e ) {
        std::cerr << "validateMenuItem has caught an exception: " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "validateMenuItem has caught an unknown exception!" << '\n';
    }
    return false;
}

- (bool)validateActionBySelector:(SEL)_selector
{
    if( !m_PC.isDisplayingCommittedData )
        return false;

    if( _selector == @selector(copy:) )
        return self.fileCopyCommandState.enabled;
    if( _selector == @selector(cut:) )
        return self.fileCutCommandState.enabled;
    if( _selector == @selector(paste:) )
        return self.filePasteCommandState.enabled;
    if( _selector == @selector(OnQuickNewFolder:) )
        return self.fileNewFolderCommandState.enabled;
    if( _selector == @selector(OnQuickNewFile:) )
        return self.fileNewFileCommandState.enabled;
    if( _selector == @selector(selectAll:) )
        return self.paneSelectAllCommandState.enabled;
    if( _selector == @selector(OnMenuInvertSelection:) )
        return self.paneInvertSelectionCommandState.enabled;
    if( _selector == @selector(onCompressItemsHere:) )
        return self.archiveCreateCommandState.enabled;
    if( _selector == @selector(onExtractArchiveHere:) )
        return self.archiveExtractCommandState.enabled;
    if( _selector == @selector(OnDuplicate:) )
        return self.fileDuplicateCommandState.enabled;
    if( _selector == @selector(OnCopyCurrentFilePath:) )
        return self.fileCopyPathCommandState.enabled;
    if( _selector == @selector(OnCalculateSizes:) )
        return self.fileCalculateSizesCommandState.enabled;
    if( _selector == @selector(OnBatchRename:) )
        return self.fileBatchRenameCommandState.enabled;
    if( _selector == @selector(OnMoveToTrash:) )
        return self.fileTrashCommandState.enabled;
    if( _selector == @selector(OnDeletePermanentlyCommand:) )
        return self.fileDeleteCommandState.enabled;
    if( _selector == @selector(OnRenameFileInPlace:) )
        return self.fileRenameCommandState.enabled;
    if( _selector == @selector(OnFileGetInfo:) )
        return self.fileGetInfoCommandState.enabled;
    if( _selector == @selector(OnFileViewCommand:) )
        return self.filePreviewCommandState.enabled;
    if( _selector == @selector(OnOpenNatively:) )
        return self.fileOpenCommandState.enabled;
    if( _selector == @selector(OnOpen:) ) {
        if( actions::Enter::UsesDefaultFileOpen(m_PC) )
            return self.fileOpenCommandState.enabled;
    }
    if( _selector == @selector(ToggleViewHiddenFiles:) )
        return self.viewToggleHiddenFilesCommandState.enabled;
    if( _selector == @selector(OnTogglePreviewPane:) )
        return self.viewTogglePreviewPaneCommandState.enabled;
    if( _selector == @selector(OnGoBack:) )
        return self.navigationBackCommandState.enabled;
    if( _selector == @selector(OnGoForward:) )
        return self.navigationForwardCommandState.enabled;
    if( _selector == @selector(OnGoToUpperDirectory:) )
        return self.navigationUpCommandState.enabled;
    if( _selector == @selector(OnRefreshPanel:) )
        return self.navigationRefreshCommandState.enabled;

    if( const auto action = ActionBySel(_selector, *m_AM) ) {
        try {
            return action->Predicate(m_PC);
        } catch( const std::exception &e ) {
            std::cerr << "validateActionBySelector has caught an exception: " << e.what() << '\n';
        } catch( ... ) {
            std::cerr << "validateActionBySelector has caught an unknown exception!" << '\n';
        }
        return false;
    }
    return false;
}

- (void)executeBySelectorIfValidOrBeep:(SEL)_selector withSender:(id)_sender
{
    if( !m_PC.isDisplayingCommittedData ) {
        NSBeep();
        return;
    }

    if( _selector == @selector(copy:) ) {
        [self executeFileCopyCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                             commandId:command_ids::FileCopy]
                                        sender:_sender];
        return;
    }
    if( _selector == @selector(cut:) ) {
        [self executeFileCutCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                            commandId:command_ids::FileCut]
                                       sender:_sender];
        return;
    }
    if( _selector == @selector(paste:) ) {
        [self executeFilePasteCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                              commandId:command_ids::FilePaste]
                                         sender:_sender];
        return;
    }
    if( _selector == @selector(OnQuickNewFolder:) ) {
        [self executeFileNewFolderCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileNewFolder]
                                               sender:_sender];
        return;
    }
    if( _selector == @selector(OnQuickNewFile:) ) {
        [self executeFileNewFileCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileNewFile]
                                             sender:_sender];
        return;
    }
    if( _selector == @selector(selectAll:) ) {
        [self executePaneSelectAllCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::PaneSelectAll]
                                              sender:_sender];
        return;
    }
    if( _selector == @selector(OnMenuInvertSelection:) ) {
        [self executePaneInvertSelectionCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::PaneInvertSelection]
                                                    sender:_sender];
        return;
    }
    if( _selector == @selector(onCompressItemsHere:) ) {
        [self executeArchiveCreateCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::ArchiveCreate]
                                              sender:_sender];
        return;
    }
    if( _selector == @selector(onExtractArchiveHere:) ) {
        [self executeArchiveExtractCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::ArchiveExtract]
                                               sender:_sender];
        return;
    }
    if( _selector == @selector(OnDuplicate:) ) {
        [self executeFileDuplicateCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileDuplicate]
                                             sender:_sender];
        return;
    }
    if( _selector == @selector(OnCopyCurrentFilePath:) ) {
        [self executeFileCopyPathCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileCopyPath]
                                            sender:_sender];
        return;
    }
    if( _selector == @selector(OnMoveToTrash:) ) {
        [self executeFileTrashCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                              commandId:command_ids::FileTrash]
                                         sender:_sender];
        return;
    }
    if( _selector == @selector(OnDeletePermanentlyCommand:) ) {
        [self executeFileDeleteCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                               commandId:command_ids::FileDelete]
                                          sender:_sender];
        return;
    }
    if( _selector == @selector(OnRenameFileInPlace:) ) {
        [self executeFileRenameCommandFromSource:[self commandInvocationSourceForSender:_sender
                                                                               commandId:command_ids::FileRename]
                                          sender:_sender];
        return;
    }
    if( _selector == @selector(OnFileGetInfo:) ) {
        [self executeFileGetInfoCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileGetInfo]
                                             sender:_sender];
        return;
    }
    if( _selector == @selector(OnFileViewCommand:) ) {
        [self executeFilePreviewCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FilePreview]
                                             sender:_sender];
        return;
    }
    if( _selector == @selector(OnOpenNatively:) ) {
        [self executeFileOpenCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::FileOpen]
                                          sender:_sender];
        return;
    }
    if( _selector == @selector(OnOpen:) ) {
        if( actions::Enter::UsesDefaultFileOpen(m_PC) ) {
            [self executeFileOpenCommandFromSource:
                      [self commandInvocationSourceForSender:_sender commandId:command_ids::FileOpen]
                                              sender:_sender];
            return;
        }
    }
    if( _selector == @selector(ToggleViewHiddenFiles:) ) {
        [self executeViewToggleHiddenFilesCommandFromSource:
                  [self commandInvocationSourceForSender:_sender
                                               commandId:command_ids::ViewToggleHiddenFiles]
                                                      sender:_sender];
        return;
    }
    if( _selector == @selector(OnTogglePreviewPane:) ) {
        [self executeViewTogglePreviewPaneCommandFromSource:
                  [self commandInvocationSourceForSender:_sender
                                               commandId:command_ids::ViewTogglePreviewPane]
                                                      sender:_sender];
        return;
    }
    if( _selector == @selector(OnGoBack:) ) {
        [self executeNavigationBackCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::NavigationBack]
                                                sender:_sender];
        return;
    }
    if( _selector == @selector(OnGoForward:) ) {
        [self executeNavigationForwardCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::NavigationForward]
                                                   sender:_sender];
        return;
    }
    if( _selector == @selector(OnGoToUpperDirectory:) ) {
        [self executeNavigationUpCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::NavigationUp]
                                              sender:_sender];
        return;
    }
    if( _selector == @selector(OnRefreshPanel:) ) {
        [self executeNavigationRefreshCommandFromSource:
                  [self commandInvocationSourceForSender:_sender commandId:command_ids::NavigationRefresh]
                                                   sender:_sender];
        return;
    }

    const auto is_valid = [self validateActionBySelector:_selector];
    if( is_valid )
        Perform(_selector, *m_AM, m_PC, _sender);
    else
        NSBeep();
}

- (nc::core::CommandState)fileOpenCommandState
{
    return [self fileOpenCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileOpenCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        return [self fileOpenCommandStateForItems:items source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "file.open state snapshot failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.open state snapshot failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileOpenCommandState();
}

- (nc::core::CommandState)fileOpenCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileOpen}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "file.open state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.open state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileOpenCommandState();
}

- (void)executeFileOpenCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        [self executeFileOpenCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        std::cerr << "file.open execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.open execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeFileOpenCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                 source:(nc::core::CommandInvocationSource)_source
                                 sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileOpen}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.open execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.open execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileGetInfoCommandState
{
    return [self fileGetInfoCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileGetInfoCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileGetInfoCommandStateForItems:items source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "file.getInfo state snapshot failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.getInfo state snapshot failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::FileGetInfo);
}

- (nc::core::CommandState)fileGetInfoCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                   source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = InspectorPresenter(m_PC) ? (__bridge void *)m_PC : nullptr,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileGetInfo}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "file.getInfo state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.getInfo state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::FileGetInfo);
}

- (void)executeFileGetInfoCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileGetInfoCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        std::cerr << "file.getInfo execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.getInfo execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeFileGetInfoCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                    source:(nc::core::CommandInvocationSource)_source
                                    sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = InspectorPresenter(m_PC) ? (__bridge void *)m_PC : nullptr,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileGetInfo}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.getInfo execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.getInfo execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)filePreviewCommandState
{
    return [self filePreviewCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)filePreviewCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self filePreviewCommandStateForItems:items source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "file.preview state snapshot failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.preview state snapshot failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::FilePreview);
}

- (nc::core::CommandState)filePreviewCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                   source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FilePreview}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "file.preview state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.preview state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::FilePreview);
}

- (void)executeFilePreviewCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFilePreviewCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        std::cerr << "file.preview execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.preview execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeFilePreviewCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                    source:(nc::core::CommandInvocationSource)_source
                                    sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FilePreview}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.preview execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.preview execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileCopyCommandState
{
    return [self fileCopyCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileCopyCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        return [self fileCopyCommandStateForItems:items source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "fileCopyCommandState snapshot has caught an exception: " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "fileCopyCommandState snapshot has caught an unknown exception!\n";
    }
    return FailedFileCopyCommandState();
}

- (nc::core::CommandState)fileCopyCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileCopy}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "fileCopyCommandState has caught an exception: " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "fileCopyCommandState has caught an unknown exception!\n";
    }

    return FailedFileCopyCommandState();
}

- (void)executeFileCopyCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        [self executeFileCopyCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        ShowExceptionAlert(e);
    } catch( ... ) {
        ShowExceptionAlert();
    }
}

- (void)executeFileCopyCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                 source:(nc::core::CommandInvocationSource)_source
                                 sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileCopy}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        ShowExceptionAlert(e);
    } catch( ... ) {
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileCutCommandState
{
    return [self fileCutCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileCutCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileCutCommandStateForItems:items source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "fileCutCommandState snapshot has caught an exception: " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "fileCutCommandState snapshot has caught an unknown exception!\n";
    }
    return FailedFileCutCommandState();
}

- (nc::core::CommandState)fileCutCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                               source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileCut}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "fileCutCommandState has caught an exception: " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "fileCutCommandState has caught an unknown exception!\n";
    }

    return FailedFileCutCommandState();
}

- (void)executeFileCutCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileCutCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        std::cerr << "file.cut execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.cut execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeFileCutCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                source:(nc::core::CommandInvocationSource)_source
                                sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileCut}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.cut execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.cut execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)filePasteCommandState
{
    return [self filePasteCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)filePasteCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FilePaste}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "file.paste state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "file.paste state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::FilePaste);
}

- (void)executeFilePasteCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FilePaste}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.paste execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.paste execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileNewFolderCommandState
{
    return [self fileNewFolderCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileNewFolderCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileNewFolder}, context).state;
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileNewFolder);
    }
}

- (void)executeFileNewFolderCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileNewFolder}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.newFolder execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.newFolder execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileNewFileCommandState
{
    return [self fileNewFileCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileNewFileCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileNewFile}, context).state;
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileNewFile);
    }
}

- (void)executeFileNewFileCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileNewFile}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.newFile execution failed (source=" << static_cast<int>(_source) << "): " << e.what()
                  << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.newFile execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)paneSelectionCommandStateForId:(std::string_view)_command_id
                                                   source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
        };
        return m_CommandRegistry->QueryState(CommandId{_command_id}, context).state;
    } catch( ... ) {
        return FailedFileMutationCommandState(_command_id);
    }
}

- (void)executePaneSelectionCommandWithId:(std::string_view)_command_id
                                    source:(nc::core::CommandInvocationSource)_source
                                    sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{_command_id}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)paneSelectAllCommandState
{
    return [self paneSelectAllCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)paneSelectAllCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    return [self paneSelectionCommandStateForId:command_ids::PaneSelectAll source:_source];
}

- (void)executePaneSelectAllCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    [self executePaneSelectionCommandWithId:command_ids::PaneSelectAll source:_source sender:_sender];
}

- (nc::core::CommandState)paneInvertSelectionCommandState
{
    return [self paneInvertSelectionCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)paneInvertSelectionCommandStateFromSource:
    (nc::core::CommandInvocationSource)_source
{
    return [self paneSelectionCommandStateForId:command_ids::PaneInvertSelection source:_source];
}

- (void)executePaneInvertSelectionCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    [self executePaneSelectionCommandWithId:command_ids::PaneInvertSelection source:_source sender:_sender];
}

- (nc::core::CommandState)payloadCommandStateForId:(std::string_view)_command_id
                                             items:(std::span<const nc::vfs::ListingItem>)_items
                                            source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{_command_id}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << _command_id << " state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << _command_id << " state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(_command_id);
}

- (void)executePayloadCommandWithId:(std::string_view)_command_id
                              items:(std::span<const nc::vfs::ListingItem>)_items
                             source:(nc::core::CommandInvocationSource)_source
                             sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{_command_id}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)archiveCreateCommandState
{
    return [self archiveCreateCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)archiveCreateCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self archiveCreateCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::ArchiveCreate);
    }
}

- (nc::core::CommandState)archiveCreateCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                      source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::ArchiveCreate items:_items source:_source];
}

- (void)executeArchiveCreateCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeArchiveCreateCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeArchiveCreateCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                       source:(nc::core::CommandInvocationSource)_source
                                       sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::ArchiveCreate items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)archiveExtractCommandState
{
    return [self archiveExtractCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)archiveExtractCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self archiveExtractCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::ArchiveExtract);
    }
}

- (nc::core::CommandState)archiveExtractCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                       source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::ArchiveExtract items:_items source:_source];
}

- (void)executeArchiveExtractCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeArchiveExtractCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeArchiveExtractCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                        source:(nc::core::CommandInvocationSource)_source
                                        sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::ArchiveExtract items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileDuplicateCommandState
{
    return [self fileDuplicateCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileDuplicateCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileDuplicateCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileDuplicate);
    }
}

- (nc::core::CommandState)fileDuplicateCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                      source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::FileDuplicate items:_items source:_source];
}

- (void)executeFileDuplicateCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileDuplicateCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileDuplicateCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                       source:(nc::core::CommandInvocationSource)_source
                                       sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::FileDuplicate items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileCopyPathCommandState
{
    return [self fileCopyPathCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileCopyPathCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileCopyPathCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileCopyPath);
    }
}

- (nc::core::CommandState)fileCopyPathCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                     source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::FileCopyPath items:_items source:_source];
}

- (void)executeFileCopyPathCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileCopyPathCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileCopyPathCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                      source:(nc::core::CommandInvocationSource)_source
                                      sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::FileCopyPath items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileCalculateSizesCommandState
{
    return [self fileCalculateSizesCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileCalculateSizesCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileCalculateSizesCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileCalculateSizes);
    }
}

- (nc::core::CommandState)fileCalculateSizesCommandStateForItems:
    (std::span<const nc::vfs::ListingItem>)_items
                                                            source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::FileCalculateSizes items:_items source:_source];
}

- (void)executeFileCalculateSizesCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileCalculateSizesCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileCalculateSizesCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                             source:(nc::core::CommandInvocationSource)_source
                                             sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::FileCalculateSizes items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileBatchRenameCommandState
{
    return [self fileBatchRenameCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileBatchRenameCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        return [self fileBatchRenameCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileBatchRename);
    }
}

- (nc::core::CommandState)fileBatchRenameCommandStateForItems:
    (std::span<const nc::vfs::ListingItem>)_items
                                                         source:(nc::core::CommandInvocationSource)_source
{
    return [self payloadCommandStateForId:command_ids::FileBatchRename items:_items source:_source];
}

- (void)executeFileBatchRenameCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntry;
        [self executeFileBatchRenameCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileBatchRenameCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                          source:(nc::core::CommandInvocationSource)_source
                                          sender:(id)_sender
{
    [self executePayloadCommandWithId:command_ids::FileBatchRename items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileDeletionCommandStateForId:(std::string_view)_command_id
                                                  items:(std::span<const nc::vfs::ListingItem>)_items
                                                 source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{_command_id}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << _command_id << " state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << _command_id << " state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(_command_id);
}

- (void)executeFileDeletionCommandWithId:(std::string_view)_command_id
                                   items:(std::span<const nc::vfs::ListingItem>)_items
                                  source:(nc::core::CommandInvocationSource)_source
                                  sender:(id)_sender
{
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{_command_id}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << _command_id << " execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)fileTrashCommandState
{
    return [self fileTrashCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileTrashCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        return [self fileTrashCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileTrash);
    }
}

- (nc::core::CommandState)fileTrashCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                 source:(nc::core::CommandInvocationSource)_source
{
    return [self fileDeletionCommandStateForId:command_ids::FileTrash items:_items source:_source];
}

- (void)executeFileTrashCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        [self executeFileTrashCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileTrashCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                  source:(nc::core::CommandInvocationSource)_source
                                  sender:(id)_sender
{
    [self executeFileDeletionCommandWithId:command_ids::FileTrash items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileDeleteCommandState
{
    return [self fileDeleteCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileDeleteCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        return [self fileDeleteCommandStateForItems:items source:_source];
    } catch( ... ) {
        return FailedFileMutationCommandState(command_ids::FileDelete);
    }
}

- (nc::core::CommandState)fileDeleteCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                  source:(nc::core::CommandInvocationSource)_source
{
    return [self fileDeletionCommandStateForId:command_ids::FileDelete items:_items source:_source];
}

- (void)executeFileDeleteCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const std::vector<VFSListingItem> items = m_PC.selectedEntriesOrFocusedEntryWithDotDot;
        [self executeFileDeleteCommandWithItems:items source:_source sender:_sender];
    } catch( ... ) {
        NSBeep();
    }
}

- (void)executeFileDeleteCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                   source:(nc::core::CommandInvocationSource)_source
                                   sender:(id)_sender
{
    [self executeFileDeletionCommandWithId:command_ids::FileDelete items:_items source:_source sender:_sender];
}

- (nc::core::CommandState)fileRenameCommandState
{
    return [self fileRenameCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)fileRenameCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    if( !m_PC.isDisplayingCommittedData )
        return FailedFileRenameCommandState();
    try {
        std::vector<VFSListingItem> items;
        if( const VFSListingItem item = m_PC.view.item )
            items.emplace_back(item);
        return [self fileRenameCommandStateForItems:items source:_source];
    } catch( const std::exception & ) {
        std::cerr << "file.rename state snapshot failed (source=" << static_cast<int>(_source) << "): exception\n";
    } catch( ... ) {
        std::cerr << "file.rename state snapshot failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileRenameCommandState();
}

- (nc::core::CommandState)fileRenameCommandStateForItems:(std::span<const nc::vfs::ListingItem>)_items
                                                  source:(nc::core::CommandInvocationSource)_source
{
    if( !m_PC.isDisplayingCommittedData )
        return FailedFileRenameCommandState();
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::FileRename}, context).state;
    } catch( const std::exception & ) {
        std::cerr << "file.rename state evaluation failed (source=" << static_cast<int>(_source)
                  << "): exception\n";
    } catch( ... ) {
        std::cerr << "file.rename state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileRenameCommandState();
}

- (void)executeFileRenameCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    if( !m_PC.isDisplayingCommittedData ) {
        NSBeep();
        return;
    }
    try {
        std::vector<VFSListingItem> items;
        if( const VFSListingItem item = m_PC.view.item )
            items.emplace_back(item);
        [self executeFileRenameCommandWithItems:items source:_source sender:_sender];
    } catch( const std::exception &e ) {
        std::cerr << "file.rename execution failed (source=" << static_cast<int>(_source) << "): exception\n";
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.rename execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeFileRenameCommandWithItems:(std::span<const nc::vfs::ListingItem>)_items
                                   source:(nc::core::CommandInvocationSource)_source
                                   sender:(id)_sender
{
    if( !m_PC.isDisplayingCommittedData ) {
        NSBeep();
        return;
    }
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .items = _items,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::FileRename}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "file.rename execution failed (source=" << static_cast<int>(_source) << "): exception\n";
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "file.rename execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandState
{
    return [self viewToggleHiddenFilesCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    if( !m_PC.isDisplayingCommittedData )
        return FailedViewToggleHiddenFilesCommandState();
    return [self viewToggleHiddenFilesCommandStateForVisibility:m_PC.data.HardFiltering().show_hidden
                                                          source:_source];
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandStateForVisibility:(std::optional<bool>)_shows_hidden_files
                                                                  source:(nc::core::CommandInvocationSource)_source
{
    if( !m_PC.isDisplayingCommittedData )
        return FailedViewToggleHiddenFilesCommandState();
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .shows_hidden_files = _shows_hidden_files,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::ViewToggleHiddenFiles}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "view.toggleHiddenFiles state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "view.toggleHiddenFiles state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedViewToggleHiddenFilesCommandState();
}

- (void)executeViewToggleHiddenFilesCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    if( !m_PC.isDisplayingCommittedData ) {
        NSBeep();
        return;
    }
    try {
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .shows_hidden_files = m_PC.data.HardFiltering().show_hidden,
        };
        const auto result =
            m_CommandRegistry->Execute(CommandId{command_ids::ViewToggleHiddenFiles}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "view.toggleHiddenFiles execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "view.toggleHiddenFiles execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)viewTogglePreviewPaneCommandState
{
    return [self viewTogglePreviewPaneCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)viewTogglePreviewPaneCommandStateFromSource:
    (nc::core::CommandInvocationSource)_source
{
    id<NCExplorerInspectorPresenting> const presenter = InspectorPresenter(m_PC);
    const std::optional<bool> visibility = presenter
                                               ? std::optional<bool>{[presenter previewPaneVisibleForPanel:m_PC]}
                                               : std::nullopt;
    return [self viewTogglePreviewPaneCommandStateForVisibility:visibility source:_source];
}

- (nc::core::CommandState)viewTogglePreviewPaneCommandStateForVisibility:
    (std::optional<bool>)_preview_pane_visible
                                                                  source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = InspectorPresenter(m_PC) ? (__bridge void *)m_PC : nullptr,
            .preview_pane_visible = _preview_pane_visible,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::ViewTogglePreviewPane}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "view.togglePreviewPane state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "view.togglePreviewPane state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedFileMutationCommandState(command_ids::ViewTogglePreviewPane);
}

- (void)executeViewTogglePreviewPaneCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        id<NCExplorerInspectorPresenting> const presenter = InspectorPresenter(m_PC);
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = presenter ? (__bridge void *)m_PC : nullptr,
            .preview_pane_visible = presenter
                                         ? std::optional<bool>{[presenter previewPaneVisibleForPanel:m_PC]}
                                         : std::nullopt,
        };
        const auto result =
            m_CommandRegistry->Execute(CommandId{command_ids::ViewTogglePreviewPane}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "view.togglePreviewPane execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "view.togglePreviewPane execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)navigationBackCommandState
{
    return [self navigationBackCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)navigationBackCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const auto availability = m_PC.history.GetNavigationAvailability();
        return [self navigationBackCommandStateForAvailability:PaneHistoryAvailability{
                                                                   .can_go_back = availability.can_go_back,
                                                                   .can_go_forward = availability.can_go_forward,
                                                               }
                                                       source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "navigation.back state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.back state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedNavigationHistoryCommandState(command_ids::NavigationBack);
}

- (nc::core::CommandState)navigationBackCommandStateForAvailability:
    (std::optional<nc::core::PaneHistoryAvailability>)_availability
                                                               source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .can_go_back = _availability ? std::optional{_availability->can_go_back} : std::nullopt,
            .can_go_forward = _availability ? std::optional{_availability->can_go_forward} : std::nullopt,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::NavigationBack}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "navigation.back state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.back state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedNavigationHistoryCommandState(command_ids::NavigationBack);
}

- (nc::core::CommandState)navigationForwardCommandState
{
    return [self navigationForwardCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)navigationForwardCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const auto availability = m_PC.history.GetNavigationAvailability();
        return [self navigationForwardCommandStateForAvailability:PaneHistoryAvailability{
                                                                      .can_go_back = availability.can_go_back,
                                                                      .can_go_forward = availability.can_go_forward,
                                                                  }
                                                          source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "navigation.forward state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.forward state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedNavigationHistoryCommandState(command_ids::NavigationForward);
}

- (nc::core::CommandState)navigationForwardCommandStateForAvailability:
    (std::optional<nc::core::PaneHistoryAvailability>)_availability
                                                                  source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .can_go_back = _availability ? std::optional{_availability->can_go_back} : std::nullopt,
            .can_go_forward = _availability ? std::optional{_availability->can_go_forward} : std::nullopt,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::NavigationForward}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "navigation.forward state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.forward state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedNavigationHistoryCommandState(command_ids::NavigationForward);
}

- (void)executeNavigationBackCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const auto availability = m_PC.history.GetNavigationAvailability();
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .can_go_back = availability.can_go_back,
            .can_go_forward = availability.can_go_forward,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::NavigationBack}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "navigation.back execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "navigation.back execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeNavigationForwardCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const auto availability = m_PC.history.GetNavigationAvailability();
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .can_go_back = availability.can_go_back,
            .can_go_forward = availability.can_go_forward,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::NavigationForward}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "navigation.forward execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "navigation.forward execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (nc::core::CommandState)navigationUpCommandState
{
    return [self navigationUpCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)navigationUpCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const auto availability = [m_PC paneNavigationAvailability];
        return [self navigationUpCommandStateForAvailability:availability ? std::optional{availability->up}
                                                                         : std::nullopt
                                                   source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "navigation.up state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.up state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedPaneNavigationCommandState(command_ids::NavigationUp);
}

- (nc::core::CommandState)navigationUpCommandStateForAvailability:
    (std::optional<nc::core::NavigationUpAvailability>)_availability
                                                       source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .navigation_up_availability = _availability,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::NavigationUp}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "navigation.up state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.up state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedPaneNavigationCommandState(command_ids::NavigationUp);
}

- (nc::core::CommandState)navigationRefreshCommandState
{
    return [self navigationRefreshCommandStateFromSource:CommandInvocationSource::Programmatic];
}

- (nc::core::CommandState)navigationRefreshCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    try {
        const auto availability = [m_PC paneNavigationAvailability];
        return [self navigationRefreshCommandStateForAvailability:
                         availability ? std::optional{availability->refresh} : std::nullopt
                                                        source:_source];
    } catch( const std::exception &e ) {
        std::cerr << "navigation.refresh state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.refresh state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedPaneNavigationCommandState(command_ids::NavigationRefresh);
}

- (nc::core::CommandState)navigationRefreshCommandStateForAvailability:
    (std::optional<nc::core::NavigationRefreshAvailability>)_availability
                                                            source:(nc::core::CommandInvocationSource)_source
{
    try {
        const CommandContext context{
            .source = _source,
            .native_target = (__bridge void *)m_PC,
            .navigation_refresh_availability = _availability,
        };
        return m_CommandRegistry->QueryState(CommandId{command_ids::NavigationRefresh}, context).state;
    } catch( const std::exception &e ) {
        std::cerr << "navigation.refresh state evaluation failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
    } catch( ... ) {
        std::cerr << "navigation.refresh state evaluation failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
    }
    return FailedPaneNavigationCommandState(command_ids::NavigationRefresh);
}

- (void)executeNavigationUpCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const auto availability = [m_PC paneNavigationAvailability];
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .navigation_up_availability = availability ? std::optional{availability->up} : std::nullopt,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::NavigationUp}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "navigation.up execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "navigation.up execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

- (void)executeNavigationRefreshCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    try {
        const auto availability = [m_PC paneNavigationAvailability];
        const CommandContext context{
            .source = _source,
            .native_sender = (__bridge const void *)_sender,
            .native_target = (__bridge void *)m_PC,
            .navigation_refresh_availability = availability ? std::optional{availability->refresh}
                                                            : std::nullopt,
        };
        const auto result = m_CommandRegistry->Execute(CommandId{command_ids::NavigationRefresh}, context);
        if( result.status != CommandRegistry::ExecutionStatus::Executed )
            NSBeep();
    } catch( const std::exception &e ) {
        std::cerr << "navigation.refresh execution failed (source=" << static_cast<int>(_source)
                  << "): " << e.what() << '\n';
        ShowExceptionAlert(e);
    } catch( ... ) {
        std::cerr << "navigation.refresh execution failed (source=" << static_cast<int>(_source)
                  << "): unknown exception\n";
        ShowExceptionAlert();
    }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (id)validRequestorForSendType:(NSString *)sendType returnType:(NSString *)returnType
{
    if( ([sendType isEqualToString:NSFilenamesPboardType] ||
         [sendType isEqualToString:(__bridge NSString *)kUTTypeFileURL]) )
        return self;

    return [super validRequestorForSendType:sendType returnType:returnType];
}

- (BOOL)writeSelectionToPasteboard:(NSPasteboard *)pboard types:(NSArray *)types
{
    if( [types containsObject:(__bridge NSString *)kUTTypeFileURL] )
        return PasteboardSupport::WriteURLSPBoard(m_PC.selectedEntriesOrFocusedEntry, pboard);
    if( [types containsObject:NSFilenamesPboardType] )
        return PasteboardSupport::WriteFilesnamesPBoard(m_PC.selectedEntriesOrFocusedEntry, pboard);
    return false;
}
#pragma clang diagnostic pop

#define PERFORM Perform(_cmd, *m_AM, m_PC, sender)

- (IBAction)OnBriefSystemOverviewCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnRefreshPanel:(id)sender
{
    [self executeNavigationRefreshCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::NavigationRefresh]
                                               sender:sender];
}
- (IBAction)OnFileInternalBigViewCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnOpen:(id)sender
{
    [self executeBySelectorIfValidOrBeep:@selector(OnOpen:) withSender:sender];
}
- (IBAction)OnGoIntoDirectory:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToUpperDirectory:(id)sender
{
    [self executeNavigationUpCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::NavigationUp]
                                          sender:sender];
}
- (IBAction)OnOpenNatively:(id)sender
{
    [self executeBySelectorIfValidOrBeep:@selector(OnOpenNatively:) withSender:sender];
}
- (IBAction)onOpenFileWith:(id)sender
{
    PERFORM;
}
- (IBAction)onAlwaysOpenFileWith:(id)sender
{
    PERFORM;
}
- (IBAction)onCompressItems:(id)sender
{
    PERFORM;
}
- (IBAction)onCompressItemsHere:(id)sender
{
    [self executeArchiveCreateCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::ArchiveCreate]
                                          sender:sender];
}
- (IBAction)onExtractArchiveHere:(id)sender
{
    [self executeArchiveExtractCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::ArchiveExtract]
                                           sender:sender];
}
- (IBAction)OnDuplicate:(id)sender
{
    [self executeFileDuplicateCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileDuplicate]
                                         sender:sender];
}
- (IBAction)OnGoBack:(id)sender
{
    [self executeNavigationBackCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::NavigationBack]
                                            sender:sender];
}
- (IBAction)OnGoForward:(id)sender
{
    [self executeNavigationForwardCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::NavigationForward]
                                               sender:sender];
}
- (IBAction)OnGoToFavoriteLocation:(id)sender
{
    PERFORM;
}
- (IBAction)OnDeleteCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnDeletePermanentlyCommand:(id)sender
{
    [self executeFileDeleteCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileDelete]
                                         sender:sender];
}
- (IBAction)OnMoveToTrash:(id)sender
{
    [self executeFileTrashCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileTrash]
                                        sender:sender];
}
- (IBAction)OnGoToSavedConnectionItem:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToFTP:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToSFTP:(id)sender
{
    PERFORM;
}
- (IBAction)onGoToWebDAV:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToNetworkShare:(id)sender
{
    PERFORM;
}
- (IBAction)OnConnectToNetworkServer:(id)sender
{
    PERFORM;
}
- (IBAction)cut:(id)sender
{
    [self executeFileCutCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileCut]
                                   sender:sender];
}
- (IBAction)copy:(id)sender
{
    [self executeFileCopyCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileCopy]
                                    sender:sender];
}
- (IBAction)OnSelectByMask:(id)sender
{
    PERFORM;
}
- (IBAction)OnDeselectByMask:(id)sender
{
    PERFORM;
}
- (IBAction)OnQuickSelectByExtension:(id)sender
{
    PERFORM;
}
- (IBAction)OnQuickDeselectByExtension:(id)sender
{
    PERFORM;
}
- (IBAction)selectAll:(id)sender
{
    [self executePaneSelectAllCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::PaneSelectAll]
                                          sender:sender];
}
- (IBAction)deselectAll:(id)sender
{
    PERFORM;
}
- (IBAction)OnMenuInvertSelection:(id)sender
{
    [self executePaneInvertSelectionCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::PaneInvertSelection]
                                                sender:sender];
}
- (IBAction)OnRenameFileInPlace:(id)sender
{
    [self executeFileRenameCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileRename]
                                      sender:sender];
}
- (IBAction)paste:(id)sender
{
    [self executeFilePasteCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FilePaste]
                                        sender:sender];
}
- (IBAction)moveItemHere:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToHome:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToDocuments:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToDesktop:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToDownloads:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToApplications:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToUtilities:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToLibrary:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToRoot:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToProcessesList:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToFolder:(id)sender
{
    PERFORM;
}
- (IBAction)OnCreateDirectoryCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnCreateDirectoryInOppositePanel:(id)sender
{
    PERFORM;
}
- (IBAction)OnQuickNewFolder:(id)sender
{
    [self executeFileNewFolderCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileNewFolder]
                                           sender:sender];
}
- (IBAction)OnQuickNewFolderWithSelection:(id)sender
{
    PERFORM;
}
- (IBAction)OnQuickNewFile:(id)sender
{
    [self executeFileNewFileCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileNewFile]
                                         sender:sender];
}
- (IBAction)OnBatchRename:(id)sender
{
    [self executeFileBatchRenameCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileBatchRename]
                                            sender:sender];
}
- (IBAction)OnOpenExtendedAttributes:(id)sender
{
    PERFORM;
}
- (IBAction)OnAddToFavorites:(id)sender
{
    PERFORM;
}
- (IBAction)OnSpotlightSearch:(id)sender
{
    PERFORM;
}
- (IBAction)OnEjectVolume:(id)sender
{
    PERFORM;
}
- (IBAction)OnCopyCurrentFileName:(id)sender
{
    PERFORM;
}
- (IBAction)OnCopyCurrentFilePath:(id)sender
{
    [self executeFileCopyPathCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileCopyPath]
                                        sender:sender];
}
- (IBAction)OnCopyCurrentFileDirectory:(id)sender
{
    PERFORM;
}
- (IBAction)OnCalculateSizes:(id)sender
{
    [self executeFileCalculateSizesCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileCalculateSizes]
                                               sender:sender];
}
- (IBAction)OnCalculateAllSizes:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleViewHiddenFiles:(id)sender
{
    [self executeViewToggleHiddenFilesCommandFromSource:
              [self commandInvocationSourceForSender:sender
                                           commandId:command_ids::ViewToggleHiddenFiles]
                                                  sender:sender];
}
- (IBAction)ToggleSeparateFoldersFromFiles:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleExtensionlessFolders:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleNaturalCollation:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleCaseInsensitiveCollation:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleCaseSensitiveCollation:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByName:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByExt:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByMTime:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortBySize:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByBTime:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByAddTime:(id)sender
{
    PERFORM;
}
- (IBAction)ToggleSortByATime:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout1:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout2:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout3:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout4:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout5:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout6:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout7:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout8:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout9:(id)sender
{
    PERFORM;
}
- (IBAction)onToggleViewLayout10:(id)sender
{
    PERFORM;
}
- (IBAction)OnOpenWithExternalEditor:(id)sender
{
    PERFORM;
}
- (IBAction)OnFileAttributes:(id)sender
{
    PERFORM;
}
- (IBAction)OnDetailedVolumeInformation:(id)sender
{
    PERFORM;
}
- (IBAction)onMainMenuPerformFindAction:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsParents:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsHistory:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsVolumes:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsFavorites:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsConnections:(id)sender
{
    PERFORM;
}
- (IBAction)OnGoToQuickListsTags:(id)sender
{
    PERFORM;
}
- (IBAction)OnCreateSymbolicLinkCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnEditSymbolicLinkCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnCreateHardLinkCommand:(id)sender
{
    PERFORM;
}
- (IBAction)OnFileGetInfo:(id)sender
{
    [self executeFileGetInfoCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FileGetInfo]
                                         sender:sender];
}
- (IBAction)OnFileViewCommand:(id)sender
{
    [self executeFilePreviewCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::FilePreview]
                                         sender:sender];
}
- (IBAction)OnTogglePreviewPane:(id)sender
{
    [self executeViewTogglePreviewPaneCommandFromSource:
              [self commandInvocationSourceForSender:sender commandId:command_ids::ViewTogglePreviewPane]
                                                  sender:sender];
}
- (IBAction)onFollowSymlink:(id)sender
{
    PERFORM;
}
- (IBAction)onShowContextMenu:(id)sender
{
    PERFORM;
}
#undef PERFORM

@end

using namespace nc::panel::actions;
namespace nc::panel {

static const actions::PanelAction *ActionBySel(SEL _sel, const PanelActionsMap &_map) noexcept
{
    const auto action = _map.find(_sel);
    return action == end(_map) ? nullptr : action->second.get();
}

static void Perform(SEL _sel, const PanelActionsMap &_map, PanelController *_target, id _sender)
{
    if( const auto action = ActionBySel(_sel, _map) ) {
        try {
            action->Perform(_target, _sender);
        } catch( std::exception &e ) {
            ShowExceptionAlert(e);
        } catch( ... ) {
            ShowExceptionAlert();
        }
    }
    else {
        std::cerr << "warning - unrecognized selector: " << NSStringFromSelector(_sel).UTF8String << '\n';
    }
}

static CommandState FailedFileCopyCommandState()
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = "file.copy state evaluation failed.",
            },
    };
}

static CommandState FailedFileOpenCommandState()
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = "file.open state evaluation failed.",
            },
    };
}

static CommandState FailedFileCutCommandState()
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = "file.cut state evaluation failed.",
            },
    };
}

static CommandState FailedFileMutationCommandState(const std::string_view _command_id)
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = std::string{_command_id} + " state evaluation failed.",
            },
    };
}

static CommandState FailedFileRenameCommandState()
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = "file.rename state evaluation failed.",
            },
    };
}

static CommandState FailedViewToggleHiddenFilesCommandState()
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = "view.toggleHiddenFiles state evaluation failed.",
            },
    };
}

static CommandState FailedNavigationHistoryCommandState(const std::string_view _command_id)
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = std::string{_command_id} + " state evaluation failed.",
            },
    };
}

static CommandState FailedPaneNavigationCommandState(const std::string_view _command_id)
{
    return CommandState{
        .visible = true,
        .enabled = false,
        .disabled_reason =
            DisabledReason{
                .code = "command.state.failed",
                .user_message_key = "commands.disabled.generic",
                .technical_message = std::string{_command_id} + " state evaluation failed.",
            },
    };
}

} // namespace nc::panel
