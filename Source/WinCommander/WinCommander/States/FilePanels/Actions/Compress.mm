// Copyright (C) 2017-2021 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Compress.h"
#include "../PanelController.h"
#include "../PanelView.h"
#include <Panel/PanelData.h>
#include "../MainWindowFilePanelState.h"
#include "../../MainWindowController.h"
#include <VFS/VFS.h>
#include <Utility/PathManip.h>
#include <Operations/Compression.h>
#include <Operations/CompressDialog.h>
#include <VFS/ProviderCapabilities.h>
#include <Base/dispatch_cpp.h>
#include <Config/Config.h>
#include "Helpers.h"
#include <tuple>
#include <algorithm>
#include <unordered_set>

namespace nc::panel::actions {

static PanelController *FindVisibleOppositeController(PanelController *_source);
static void FocusResult(PanelController *_target, const std::shared_ptr<nc::ops::Compression> &_op);
static void PresentStaleArchiveAlert(PanelController *_target);

static const auto g_DeselectConfigFlag = "filePanel.general.deselectItemsAfterFileOperations";

CompressBase::CompressBase(nc::config::Config &_config) : m_Config{_config}
{
}

void CompressBase::AddDeselectorIfNeeded(nc::ops::Operation &_operation, PanelController *_target) const
{
    if( !ShouldAutomaticallyDeselect() )
        return;

    const auto deselector = std::make_shared<const DeselectorViaOpNotification>(_target);
    _operation.SetItemStatusCallback([deselector](nc::ops::ItemStateReport _report) { deselector->Handle(_report); });
}

bool CompressBase::ShouldAutomaticallyDeselect() const
{
    return m_Config.GetBool(g_DeselectConfigFlag);
}

ArchiveCreateSubmissionResult EvaluateArchiveCreateSubmission(const std::span<const VFSListingItem> _items,
                                                              PanelController *_target)
{
    if( !_target )
        return ArchiveCreateSubmissionResult::PaneUnavailable;
    if( !_target.mainWindowController )
        return ArchiveCreateSubmissionResult::WindowUnavailable;
    if( _target.isDoingBackgroundLoading )
        return ArchiveCreateSubmissionResult::Loading;
    if( _items.empty() )
        return ArchiveCreateSubmissionResult::SelectionUnavailable;
    if( std::ranges::any_of(_items, [](const VFSListingItem &_item) { return _item.IsDotDot(); }) )
        return ArchiveCreateSubmissionResult::ParentEntryUnsupported;

    try {
        const VFSListingPtr listing = _target.data.ListingPtr();
        if( !_target.isUniform || !_target.vfs || !listing )
            return ArchiveCreateSubmissionResult::DestinationUnavailable;
        if( std::ranges::any_of(_items, [&](const VFSListingItem &_item) {
                return !_item.Host() || _item.Listing().get() != listing.get();
            }) ) {
            return ArchiveCreateSubmissionResult::StaleContext;
        }

        std::unordered_set<std::string> top_level_names;
        for( const VFSListingItem &item : _items ) {
            const vfs::ProviderCapabilities source =
                vfs::ProviderCapabilitiesResolver::Resolve(*item.Host(), item.Directory());
            if( !source.can_read || (!item.IsReg() && !item.IsDir() && !item.IsSymlink()) )
                return ArchiveCreateSubmissionResult::SourceUnreadable;
            NSString *const filename = item.FilenameNS();
            NSString *const normalized = filename.precomposedStringWithCanonicalMapping.lowercaseString;
            const char *const normalized_utf8 = normalized.UTF8String;
            if( normalized.length == 0 || !normalized_utf8 || !top_level_names.emplace(normalized_utf8).second )
                return ArchiveCreateSubmissionResult::SourceNameCollision;
        }

        const std::string destination_path = _target.currentDirectoryPath;
        if( !_target.vfs->IsWritableAtPath(destination_path) )
            return ArchiveCreateSubmissionResult::DestinationReadOnly;
        const vfs::ProviderCapabilities destination =
            vfs::ProviderCapabilitiesResolver::Resolve(*_target.vfs, destination_path);
        if( !destination.can_create_file )
            return ArchiveCreateSubmissionResult::ProviderUnsupported;
    } catch( ... ) {
        return ArchiveCreateSubmissionResult::DestinationUnavailable;
    }
    return ArchiveCreateSubmissionResult::Presented;
}

ArchiveCreateSubmissionResult PresentArchiveCreate(const std::span<const VFSListingItem> _items,
                                                   PanelController *_target,
                                                   nc::config::Config &_config)
{
    const ArchiveCreateSubmissionResult live = EvaluateArchiveCreateSubmission(_items, _target);
    if( live != ArchiveCreateSubmissionResult::Presented )
        return live;

    const std::vector<VFSListingItem> entries{_items.begin(), _items.end()};
    const VFSListingPtr source_listing = _target.data.ListingPtr();
    const unsigned long source_generation = _target.dataGeneration;
    const VFSHostPtr destination_vfs = _target.vfs;
    const std::string initial_destination = _target.currentDirectoryPath;
    NCMainWindowController *const window_controller = _target.mainWindowController;
    auto dialog = [[NCOpsCompressDialog alloc] initWithItems:entries
                                              destinationVFS:destination_vfs
                                          initialDestination:initial_destination];
    __weak PanelController *weak_target = _target;
    __weak NCMainWindowController *weak_window_controller = window_controller;
    auto *const config = &_config;
    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;
      PanelController *const target = weak_target;
      NCMainWindowController *const current_window_controller = weak_window_controller;
      if( !target || !current_window_controller || target.mainWindowController != current_window_controller ||
          target.dataGeneration != source_generation || target.data.ListingPtr() != source_listing ||
          target.vfs != destination_vfs ) {
          PresentStaleArchiveAlert(target);
          return;
      }

      const ArchiveCreateSubmissionResult current = EvaluateArchiveCreateSubmission(entries, target);
      if( current != ArchiveCreateSubmissionResult::Presented ) {
          PresentStaleArchiveAlert(target);
          return;
      }
      try {
          if( dialog.destination.empty() || !destination_vfs->IsWritableAtPath(dialog.destination) ||
              !vfs::ProviderCapabilitiesResolver::Resolve(*destination_vfs, dialog.destination).can_create_file ) {
              PresentStaleArchiveAlert(target);
              return;
          }
      } catch( ... ) {
          PresentStaleArchiveAlert(target);
          return;
      }

      auto op = std::make_shared<nc::ops::Compression>(entries, dialog.destination, destination_vfs, dialog.password);
      const auto weak_op = std::weak_ptr<nc::ops::Compression>{op};
      __weak PanelController *weak_focus_target = target;
      op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [weak_focus_target, weak_op] {
          FocusResult(static_cast<PanelController *>(weak_focus_target), weak_op.lock());
      });
      CompressBase{*config}.AddDeselectorIfNeeded(*op, target);
      [current_window_controller enqueueOperation:op];
    };
    [window_controller beginSheet:dialog.window completionHandler:handler];
    return ArchiveCreateSubmissionResult::Presented;
}

CompressHere::CompressHere(nc::config::Config &_config) : CompressBase(_config)
{
}

bool CompressHere::Predicate(PanelController *_target) const
{
    if( !_target )
        return false;
    const auto entries = _target.selectedEntriesOrFocusedEntry;
    return EvaluateArchiveCreateSubmission(entries, _target) == ArchiveCreateSubmissionResult::Presented;
}

void CompressHere::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto entries = _target.selectedEntriesOrFocusedEntry;
    std::ignore = PresentArchiveCreate(entries, _target, Config());
}

CompressToOpposite::CompressToOpposite(nc::config::Config &_config) : CompressBase(_config)
{
}

bool CompressToOpposite::Predicate(PanelController *_target) const
{
    const auto i = _target.view.item;
    if( !i )
        return false;
    if( i.IsDotDot() && _target.data.Stats().selected_entries_amount == 0 )
        return false;

    auto opposite = FindVisibleOppositeController(_target);
    if( !opposite )
        return false;

    return opposite.isUniform && opposite.vfs->IsWritable();
}

void CompressToOpposite::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto opposite_panel = FindVisibleOppositeController(_target);
    if( !opposite_panel.isUniform || !opposite_panel.vfs->IsWritable() )
        return;

    auto entries = _target.selectedEntriesOrFocusedEntry;
    if( entries.empty() )
        return;

    auto dialog = [[NCOpsCompressDialog alloc] initWithItems:entries
                                              destinationVFS:opposite_panel.vfs
                                          initialDestination:opposite_panel.currentDirectoryPath];

    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      auto op =
          std::make_shared<nc::ops::Compression>(entries, dialog.destination, opposite_panel.vfs, dialog.password);
      const auto weak_op = std::weak_ptr<nc::ops::Compression>{op};
      __weak PanelController *weak_target = opposite_panel;
      op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [weak_target, weak_op] {
          FocusResult(static_cast<PanelController *>(weak_target), weak_op.lock());
      });

      AddDeselectorIfNeeded(*op, _target);

      [_target.mainWindowController enqueueOperation:op];
    };

    [_target.mainWindowController beginSheet:dialog.window completionHandler:handler];
}

context::CompressHere::CompressHere(nc::config::Config &_config, const std::vector<VFSListingItem> &_items)
    : CompressBase(_config), m_Items(_items)
{
}

bool context::CompressHere::Predicate(PanelController *_target) const
{
    return EvaluateArchiveCreateSubmission(m_Items, _target) == ArchiveCreateSubmissionResult::Presented;
}

bool context::CompressHere::ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const
{
    if( m_Items.size() > 1 )
        _item.title =
            [NSString stringWithFormat:NSLocalizedStringFromTable(
                                           @"Compress %lu Items", @"FilePanelsContextMenu", "Compress some items here"),
                                       m_Items.size()];
    else
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(@"Compress \u201c%@\u201d",
                                                                            @"FilePanelsContextMenu",
                                                                            "Compress one item here"),
                                                 m_Items.front().DisplayNameNS()];

    return Predicate(_target);
}

void context::CompressHere::Perform(PanelController *_target, id /*_sender*/) const
{
    std::ignore = PresentArchiveCreate(m_Items, _target, Config());
}

context::CompressToOpposite::CompressToOpposite(nc::config::Config &_config, const std::vector<VFSListingItem> &_items)
    : CompressBase(_config), m_Items(_items)
{
}

bool context::CompressToOpposite::Predicate(PanelController *_target) const
{
    auto opposite = FindVisibleOppositeController(_target);
    if( !opposite )
        return false;

    return opposite.isUniform && opposite.vfs->IsWritable();
}

bool context::CompressToOpposite::ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const
{
    if( m_Items.size() > 1 )
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(@"Compress %lu Items in Opposite Panel",
                                                                            @"FilePanelsContextMenu",
                                                                            "Compress some items"),
                                                 m_Items.size()];
    else
        _item.title =
            [NSString stringWithFormat:NSLocalizedStringFromTable(@"Compress \u201c%@\u201d in Opposite Panel",
                                                                  @"FilePanelsContextMenu",
                                                                  "Compress one item"),
                                       m_Items.front().DisplayNameNS()];

    return Predicate(_target);
}

void context::CompressToOpposite::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto opposite_panel = FindVisibleOppositeController(_target);
    if( !opposite_panel.isUniform || !opposite_panel.vfs->IsWritable() )
        return;

    auto entries = m_Items;
    auto op = std::make_shared<nc::ops::Compression>(
        std::move(entries), opposite_panel.currentDirectoryPath, opposite_panel.vfs);
    const auto weak_op = std::weak_ptr<nc::ops::Compression>{op};
    __weak PanelController *weak_target = opposite_panel;
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [weak_target, weak_op] {
        FocusResult(static_cast<PanelController *>(weak_target), weak_op.lock());
    });

    AddDeselectorIfNeeded(*op, _target);

    [_target.mainWindowController enqueueOperation:op];
}

static PanelController *FindVisibleOppositeController(PanelController *_source)
{
    auto state = _source.state;
    if( !state.bothPanelsAreVisible )
        return nil;
    if( [state isLeftController:_source] )
        return state.rightPanelController;
    if( [state isRightController:_source] )
        return state.leftPanelController;
    return nil;
}

static void FocusResult(PanelController *_target, const std::shared_ptr<nc::ops::Compression> &_op)
{
    if( !_target || !_op )
        return;

    if( dispatch_is_main_queue() ) {
        const auto result_path = std::filesystem::path(_op->ArchivePath());
        const auto directory = EnsureTrailingSlash(result_path.parent_path());
        const auto filename = result_path.filename().native();
        if( _target.isUniform && _target.currentDirectoryPath == directory ) {
            [_target refreshPanel];
            nc::panel::DelayedFocusing req;
            req.filename = filename;
            [_target scheduleDelayedFocusing:req];
        }
    }
    else
        dispatch_to_main_queue([_target, _op] { FocusResult(_target, _op); });
}

static void PresentStaleArchiveAlert(PanelController *_target)
{
    if( !_target || !_target.mainWindowController ) {
        NSBeep();
        return;
    }
    NSAlert *const alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = NSLocalizedString(@"commands.file.mutation.disabled.stale",
                                          "Stale archive creation message");
    [alert addButtonWithTitle:NSLocalizedString(@"OK", "Alert confirmation button")];
    [alert beginSheetModalForWindow:_target.mainWindowController.window completionHandler:nil];
}

} // namespace nc::panel::actions
