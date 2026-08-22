// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Delete.h"
#include "CopyFile.h"
#include "../PanelController.h"
#include "../MainWindowFilePanelState.h"
#include <Utility/NativeFSManager.h>
#include <Base/algo.h>
#include <Panel/PanelData.h>
#include "../PanelView.h"
#include <Operations/Deletion.h>
#include <Operations/DeletionDialog.h>
#include <VFS/ProviderCapabilities.h>
#include "../../MainWindowController.h"
#include <ankerl/unordered_dense.h>
#include <Base/dispatch_cpp.h>

#include <algorithm>
#include <sys/stat.h>

namespace nc::panel::actions {

static bool CommonDeletePredicate(PanelController *_target);
static bool AllAreNative(const std::vector<VFSListingItem> &_c);
static ankerl::unordered_dense::set<std::string> ExtractDirectories(const std::vector<VFSListingItem> &_c);
static bool TryTrash(const std::vector<VFSListingItem> &_c, utility::NativeFSManager &_fsman);
static void AddPanelRefreshEpilog(PanelController *_target, nc::ops::Operation &_operation);
static bool SupportsDeletion(std::span<const VFSListingItem> _items, bool _trash);
static bool DeletionContextIsCurrent(std::span<const VFSListingItem> _items,
                                     PanelController *_target,
                                     bool _trash);
static void PresentStaleDeletionAlert(PanelController *_target);
static void PresentDeleteEligibilityUnavailableAlert(PanelController *_target);

namespace reviewed_delete {
namespace {

/** Everything the policy agrees on before asking the provider its own question. */
bool IsReviewedDeleteShape(const VFSListingItem &_item) noexcept
{
    return _item && _item.IsReg() && _item.Host() && _item.Host()->IsNativeFS();
}

} // namespace

Selection Select(const VFSListingItem &_item) noexcept
{
    try {
        if( !IsReviewedDeleteShape(_item) )
            return Selection::Legacy;
        switch( _item.Host()->ConditionalDeletePathSupport(_item.Path()) ) {
            case nc::vfs::ProviderConditionalDeletePathSupport::SameVolumeUnlink:
                return Selection::Reviewed;
            case nc::vfs::ProviderConditionalDeletePathSupport::Unsupported:
                return Selection::Legacy;
            case nc::vfs::ProviderConditionalDeletePathSupport::Unavailable:
                return Selection::Reject;
        }
    } catch( ... ) {
        return Selection::Reject;
    }
    return Selection::Reject;
}

Selection SelectBatch(const std::vector<VFSListingItem> &_items) noexcept
{
    if( _items.empty() )
        return Selection::Legacy;

    // Every item is asked, even once one has already answered legacy: a `Reject` further down is an
    // eligibility question the provider could not answer, and stopping early would turn it into a
    // silent legacy delete - the one outcome the single-item rule exists to refuse.
    bool any_legacy = false;
    for( const auto &item : _items ) {
        switch( Select(item) ) {
            case Selection::Reject:
                return Selection::Reject;
            case Selection::Legacy:
                any_legacy = true;
                break;
            case Selection::Reviewed:
                break;
        }
    }
    return any_legacy ? Selection::Legacy : Selection::Reviewed;
}

} // namespace reviewed_delete

Delete::Delete(nc::utility::NativeFSManager &_nat_fsman, bool _permanently)
    : m_NativeFSManager{_nat_fsman}, m_Permanently(_permanently)
{
}

bool Delete::Predicate(PanelController *_target) const
{
    return CommonDeletePredicate(_target);
}

void Delete::Perform(PanelController *_target, id /*_sender*/) const
{
    auto items = to_shared_ptr(_target.selectedEntriesOrFocusedEntry);
    if( items->empty() )
        return;

    const auto sheet = [[NCOpsDeletionDialog alloc] initWithItems:items];
    if( AllAreNative(*items) ) {
        const auto try_trash = TryTrash(*items, m_NativeFSManager);
        sheet.allowMoveToTrash = try_trash;
        sheet.defaultType = [&] {
            if( m_Permanently )
                return nc::ops::DeletionType::Permanent;
            else
                return try_trash ? nc::ops::DeletionType::Trash : nc::ops::DeletionType::Permanent;
        }();
    }
    else {
        sheet.allowMoveToTrash = false;
        sheet.defaultType = nc::ops::DeletionType::Permanent;
    }

    auto sheet_handler = ^(NSModalResponse returnCode) {
      if( returnCode == NSModalResponseOK ) {
          const auto operation = std::make_shared<nc::ops::Deletion>(std::move(*items), sheet.resultType);
          AddPanelRefreshEpilog(_target, *operation);
          [_target.mainWindowController enqueueOperation:operation];
      }
    };

    [_target.mainWindowController beginSheet:sheet.window completionHandler:sheet_handler];
}

MoveToTrash::MoveToTrash(nc::utility::NativeFSManager &_nat_fsman) : m_NativeFSManager{_nat_fsman}
{
}

bool SubmitItemsToTrash(const std::span<const VFSListingItem> _items, PanelController *_target)
{
    if( !DeletionContextIsCurrent(_items, _target, true) )
        return false;

    std::vector<VFSListingItem> items{_items.begin(), _items.end()};
    const auto operation = std::make_shared<nc::ops::Deletion>(std::move(items), nc::ops::DeletionType::Trash);
    AddPanelRefreshEpilog(_target, *operation);
    [_target.mainWindowController enqueueOperation:operation];
    return true;
}

bool PresentPermanentDeletion(const std::span<const VFSListingItem> _items,
                              PanelController *_target)
{
    if( !DeletionContextIsCurrent(_items, _target, false) )
        return false;

    // The reviewed submission machinery is wired to `MainWindowFilePanelState` only - it needs a
    // per-window `OperationSubmissionGate`, which only that hosting state owns today; an
    // Explorer-hosted panel has no such gate to acquire a submission ticket from. Falls through to the
    // legacy dialog below unconditionally, the same named scope boundary `Trash` has everywhere.
    MainWindowFilePanelState *const reviewed_state =
        [_target.state isKindOfClass:MainWindowFilePanelState.class]
            ? static_cast<MainWindowFilePanelState *>(_target.state)
            : nil;
    if( reviewed_state ) {
        std::vector<VFSListingItem> reviewed_candidates{_items.begin(), _items.end()};
        switch( reviewed_delete::SelectBatch(reviewed_candidates) ) {
            case reviewed_delete::Selection::Reviewed: {
                __weak PanelController *weak_target = _target;
                auto intent_is_current = [weak_target, items = reviewed_candidates]() {
                    PanelController *const target = weak_target;
                    return target && DeletionContextIsCurrent(std::span<const VFSListingItem>{items}, target, false);
                };
                auto refresh_panel = [weak_target] {
                    if( PanelController *const target = weak_target )
                        [target hintAboutFilesystemChange];
                };
                SubmitReviewedDelete(reviewed_state,
                                     _target,
                                     std::move(reviewed_candidates),
                                     std::move(intent_is_current),
                                     std::move(refresh_panel));
                return true;
            }
            case reviewed_delete::Selection::Reject:
                // An eligibility question the provider could not answer must never be quietly
                // downgraded into "delete it the old way" - the same rule `reviewed_move::Select`
                // already applies, restated here rather than silently falling through below.
                PresentDeleteEligibilityUnavailableAlert(_target);
                return true;
            case reviewed_delete::Selection::Legacy:
                break;
        }
    }

    auto items = std::make_shared<std::vector<VFSListingItem>>(_items.begin(), _items.end());
    const auto sheet = [[NCOpsDeletionDialog alloc] initWithItems:items];
    sheet.allowMoveToTrash = false;
    sheet.defaultType = nc::ops::DeletionType::Permanent;

    __weak PanelController *weak_target = _target;
    auto sheet_handler = ^(NSModalResponse returnCode) {
      PanelController *const target = weak_target;
      if( returnCode != NSModalResponseOK )
          return;
      if( !DeletionContextIsCurrent(std::span<const VFSListingItem>{*items}, target, false) ) {
          PresentStaleDeletionAlert(target);
          return;
      }

      const auto operation = std::make_shared<nc::ops::Deletion>(*items, nc::ops::DeletionType::Permanent);
      AddPanelRefreshEpilog(target, *operation);
      [target.mainWindowController enqueueOperation:operation];
    };
    [_target.mainWindowController beginSheet:sheet.window completionHandler:sheet_handler];
    return true;
}

bool MoveToTrash::Predicate(PanelController *_target) const
{
    return CommonDeletePredicate(_target);
}

void MoveToTrash::Perform(PanelController *_target, id _sender) const
{
    auto items = _target.selectedEntriesOrFocusedEntry;

    if( !AllAreNative(items) ) {
        // instead of trying to silently reap files on VFS like FTP
        // (that means we'll erase it, not move to trash),
        // forward the request as a regular F8 delete
        Delete{m_NativeFSManager, false}.Perform(_target, _sender);
        return;
    }

    if( !TryTrash(items, m_NativeFSManager) ) {
        // if user called MoveToTrash by cmd+backspace but there's no trash on this volume:
        // show a dialog and ask him to delete a file permanently
        Delete{m_NativeFSManager, true}.Perform(_target, _sender);
        return;
    }

    const auto operation = std::make_shared<nc::ops::Deletion>(std::move(items), nc::ops::DeletionType::Trash);
    AddPanelRefreshEpilog(_target, *operation);
    [_target.mainWindowController enqueueOperation:operation];
}

context::MoveToTrash::MoveToTrash(const std::vector<VFSListingItem> &_items) : m_Items(_items)
{
    m_AllAreNative = AllAreNative(m_Items);
}

bool context::MoveToTrash::Predicate(PanelController * /*_target*/) const
{
    return m_AllAreNative;
}

void context::MoveToTrash::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto operation = std::make_shared<nc::ops::Deletion>(m_Items, nc::ops::DeletionType::Trash);
    AddPanelRefreshEpilog(_target, *operation);
    [_target.mainWindowController enqueueOperation:operation];
}

context::DeletePermanently::DeletePermanently(const std::vector<VFSListingItem> &_items) : m_Items(_items)
{
    m_AllWriteable = std::ranges::all_of(m_Items, [](const auto &i) { return i.Host()->IsWritable(); });
}

bool context::DeletePermanently::Predicate(PanelController * /*_target*/) const
{
    return m_AllWriteable;
}

void context::DeletePermanently::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto operation = std::make_shared<nc::ops::Deletion>(m_Items, nc::ops::DeletionType::Permanent);
    AddPanelRefreshEpilog(_target, *operation);
    [_target.mainWindowController enqueueOperation:operation];
}

static bool CommonDeletePredicate(PanelController *_target)
{
    auto i = _target.view.item;
    if( !i || !i.Host()->IsWritable() )
        return false;
    return !i.IsDotDot() || _target.data.Stats().selected_entries_amount > 0;
}

static bool SupportsDeletion(const std::span<const VFSListingItem> _items, const bool _trash)
{
    if( _items.empty() )
        return false;
    return std::ranges::all_of(_items, [_trash](const VFSListingItem &_item) {
        if( _item.IsDotDot() || !_item.Host() )
            return false;
        const vfs::ProviderCapabilities capabilities =
            vfs::ProviderCapabilitiesResolver::Resolve(*_item.Host(), _item.Directory());
        return _trash ? capabilities.can_trash : capabilities.can_delete_permanently;
    });
}

static bool DeletionContextIsCurrent(const std::span<const VFSListingItem> _items,
                                     PanelController *_target,
                                     const bool _trash)
{
    if( !_target || !_target.mainWindowController || _target.isDoingBackgroundLoading ||
        !SupportsDeletion(_items, _trash) ) {
        return false;
    }

    const VFSListing &current_listing = _target.data.Listing();
    return std::ranges::all_of(_items, [&](const VFSListingItem &_item) {
        if( _item.Listing().get() != &current_listing )
            return false;
        if( !_item.Host()->IsNativeFS() )
            return true;
        if( !_item.HasInode() )
            return false;
        struct stat current {};
        return lstat(_item.Path().c_str(), &current) == 0 &&
               static_cast<uint64_t>(current.st_ino) == _item.Inode();
    });
}

static void PresentStaleDeletionAlert(PanelController *_target)
{
    if( !_target || !_target.mainWindowController ) {
        NSBeep();
        return;
    }
    const auto alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = NSLocalizedString(@"commands.file.mutation.disabled.stale",
                                          "Stale deletion review message");
    [alert addButtonWithTitle:NSLocalizedString(@"OK", "Alert confirmation button")];
    [alert beginSheetModalForWindow:_target.mainWindowController.window completionHandler:nil];
}

static void PresentDeleteEligibilityUnavailableAlert(PanelController *_target)
{
    if( !_target || !_target.mainWindowController ) {
        NSBeep();
        return;
    }
    const auto alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"Delete validation unavailable";
    alert.informativeText =
        @"The storage provider could not establish whether this delete is eligible. The delete was not started.";
    [alert addButtonWithTitle:NSLocalizedString(@"OK", "Alert confirmation button")];
    [alert beginSheetModalForWindow:_target.mainWindowController.window completionHandler:nil];
}

static bool AllAreNative(const std::vector<VFSListingItem> &_c)
{
    return std::ranges::all_of(_c, [&](auto &i) { return i.Host()->IsNativeFS(); });
}

static ankerl::unordered_dense::set<std::string> ExtractDirectories(const std::vector<VFSListingItem> &_c)
{
    ankerl::unordered_dense::set<std::string> directories;
    for( const auto &i : _c )
        directories.emplace(i.Directory());
    return directories;
}

static bool TryTrash(const std::vector<VFSListingItem> &_c, utility::NativeFSManager &_fsman)
{
    const auto directories = ExtractDirectories(_c);

    const bool all_have_trash = std::ranges::all_of(directories, [&](const std::string &dir) {
        if( auto vol = _fsman.VolumeFromPath(dir); vol && vol->interfaces.has_trash )
            return true;
        return false;
    });

    // if we already know that each volume have a trash folder - just say yes
    if( all_have_trash )
        return true;

    // otherwise, speculate a bit and try doing trash on locally-mounted volumes as well
    const bool all_are_local = std::ranges::all_of(directories, [&](const std::string &dir) {
        if( auto vol = _fsman.VolumeFromPath(dir); vol && vol->mount_flags.local )
            return true;
        return false;
    });
    return all_are_local;
}

static void AddPanelRefreshEpilog(PanelController *_target, nc::ops::Operation &_operation)
{
    __weak PanelController *weak_panel = _target;
    _operation.ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [=] {
        dispatch_to_main_queue([=] {
            if( PanelController *const strong_pc = weak_panel )
                [strong_pc hintAboutFilesystemChange];
        });
    });
}

} // namespace nc::panel::actions
