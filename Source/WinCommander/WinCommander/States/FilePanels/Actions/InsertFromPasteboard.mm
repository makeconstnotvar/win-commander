// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include <VFS/Native.h>
#include <VFS/ProviderCapabilities.h>
#include <Utility/PathManip.h>
#include "../PanelController.h"
#include "../PanelAux.h"
#include "../MainWindowFilePanelState.h"
#include "InsertFromPasteboard.h"
#include "../Helpers/Pasteboard.h"
#include <Operations/Copying.h>
#include "../../MainWindowController.h"
#include <Utility/ObjCpp.h>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>

namespace nc::panel::actions {

// currently supports only info from NSFilenamesPboardType.
// perhaps it would be good to add support of URLS at least.
// or even with custom Duck Commander structures used in drag&drop system

static std::vector<VFSListingItem> FetchVFSListingsItemsFromPaths(NSArray *_input, vfs::NativeHost &_native_host)
{
    std::vector<VFSListingItem> result;
    for( NSString *ns_filepath in _input ) {
        if( !objc_cast<NSString>(ns_filepath) )
            continue; // guard against malformed input

        if( const char *filepath = ns_filepath.fileSystemRepresentation ) {
            if( const std::expected<VFSListingPtr, Error> listing = _native_host.FetchSingleItemListing(filepath, 0) )
                result.emplace_back((*listing)->Item(0));
        }
    }
    return result;
}

struct PasteboardSourceItems {
    std::vector<VFSListingItem> items;
    size_t listed_paths = 0;
};

static PasteboardSourceItems FetchVFSListingsItemsFromPasteboard(NSPasteboard *_pasteboard,
                                                                vfs::NativeHost &_native_host)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

    // check what's inside pasteboard
    if( [_pasteboard availableTypeFromArray:@[NSFilenamesPboardType]] ) {
        // input should be an array of filepaths as NSStrings
        auto filepaths = objc_cast<NSArray>([_pasteboard propertyListForType:NSFilenamesPboardType]);

        // currently fetching listings synchronously, which is BAAAD
        // (but we're on native vfs, at least for now)
        if( !filepaths )
            return {};
        return {.items = FetchVFSListingsItemsFromPaths(filepaths, _native_host),
                .listed_paths = static_cast<size_t>(filepaths.count)};
    }
    // TODO: reading from URL pasteboard?
    return {};

#pragma clang diagnostic pop
}

struct NativeSourceIdentity {
    std::string path;
    dev_t device = 0;
    ino_t inode = 0;
};

static std::optional<std::vector<NativeSourceIdentity>> CaptureSourceIdentities(
    const std::vector<VFSListingItem> &_items)
{
    std::vector<NativeSourceIdentity> identities;
    identities.reserve(_items.size());
    for( const VFSListingItem &item : _items ) {
        struct stat st {};
        if( lstat(item.Path().c_str(), &st) != 0 )
            return std::nullopt;
        identities.emplace_back(NativeSourceIdentity{.path = item.Path(), .device = st.st_dev, .inode = st.st_ino});
    }
    return identities;
}

static bool OriginalSourceItemsWereMoved(const std::vector<NativeSourceIdentity> &_identities)
{
    return std::ranges::all_of(_identities, [](const NativeSourceIdentity &_identity) {
        struct stat st {};
        if( lstat(_identity.path.c_str(), &st) != 0 )
            return errno == ENOENT || errno == ENOTDIR;
        return st.st_dev != _identity.device || st.st_ino != _identity.inode;
    });
}

static PasteSubmissionResult PasteOrMove(PanelController *_target,
                                         bool _paste,
                                         vfs::NativeHost &_native_host,
                                         NSPasteboard *_pasteboard,
                                         std::optional<PasteboardCutToken> _cut_token = std::nullopt,
                                         std::optional<PasteboardFileListToken> _file_list_token = std::nullopt)
{
    const auto release_move_claim = [&] {
        if( _cut_token )
            PasteboardSupport::ReleaseCut(_pasteboard, *_cut_token);
        if( _file_list_token )
            PasteboardSupport::ReleaseFileListMove(_pasteboard, *_file_list_token);
    };

    if( !_pasteboard )
        return PasteSubmissionResult::ClipboardUnavailable;
    if( !_target ) {
        release_move_claim();
        return PasteSubmissionResult::PaneUnavailable;
    }

    auto *const window_controller = _target.mainWindowController;
    VFSHostPtr destination_vfs;
    std::string destination_path;
    try {
        if( !window_controller || _target.isDoingBackgroundLoading || !_target.isUniform || !_target.vfs ) {
            release_move_claim();
            return !window_controller ? PasteSubmissionResult::WindowUnavailable
                                      : PasteSubmissionResult::DestinationUnavailable;
        }
        destination_vfs = _target.vfs;
        destination_path = _target.currentDirectoryPath;
        if( !vfs::ProviderCapabilitiesResolver::Resolve(*destination_vfs, destination_path).can_write ) {
            release_move_claim();
            return PasteSubmissionResult::DestinationReadOnly;
        }
    } catch( ... ) {
        release_move_claim();
        return PasteSubmissionResult::DestinationUnavailable;
    }

    auto pasteboard_items = FetchVFSListingsItemsFromPasteboard(_pasteboard, _native_host);

    if( pasteboard_items.items.empty() || pasteboard_items.items.size() != pasteboard_items.listed_paths ) {
        release_move_claim();
        return PasteSubmissionResult::SourceUnavailable;
    }

    try {
        if( _target.mainWindowController != window_controller || _target.isDoingBackgroundLoading ||
            !_target.isUniform || !_target.vfs || _target.vfs != destination_vfs ||
            _target.currentDirectoryPath != destination_path ||
            !vfs::ProviderCapabilitiesResolver::Resolve(*destination_vfs, destination_path).can_write ) {
            release_move_claim();
            return PasteSubmissionResult::DestinationUnavailable;
        }
    } catch( ... ) {
        release_move_claim();
        return PasteSubmissionResult::DestinationUnavailable;
    }

    // Revalidate after reading the paths. An external clipboard replacement must never turn
    // a newly supplied file list into a destructive move.
    if( !_paste && _cut_token ) {
        const auto current_token = PasteboardSupport::CurrentCutToken(_pasteboard);
        if( !current_token || *current_token != *_cut_token ) {
            release_move_claim();
            return PasteSubmissionResult::ClipboardChanged;
        }
    }
    if( !_paste && _file_list_token &&
        !PasteboardSupport::IsFileListMoveClaimCurrent(_pasteboard, *_file_list_token) ) {
        release_move_claim();
        return PasteSubmissionResult::ClipboardChanged;
    }

    std::optional<std::vector<NativeSourceIdentity>> source_identities;
    if( !_paste ) {
        source_identities = CaptureSourceIdentities(pasteboard_items.items);
        if( !source_identities ) {
            release_move_claim();
            return PasteSubmissionResult::SourceUnavailable;
        }
    }

    auto opts = MakeDefaultFileCopyOptions();
    opts.docopy = _paste;
    __weak PanelController *wpc = _target;
    const auto op = std::make_shared<nc::ops::Copying>(
        std::move(pasteboard_items.items), destination_path, destination_vfs, opts);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [=] {
        dispatch_to_main_queue([=] {
            if( PanelController *const pc = wpc )
                [pc refreshPanel];
        });
    });
    if( !_paste ) {
        op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion,
                              [pasteboard = _pasteboard,
                               cut_token = _cut_token,
                               file_list_token = _file_list_token,
                               identities = std::move(*source_identities)] {
          const bool fully_moved = OriginalSourceItemsWereMoved(identities);
          dispatch_to_main_queue([pasteboard, cut_token, file_list_token, fully_moved] {
              if( cut_token ) {
                  if( fully_moved )
                      PasteboardSupport::ConsumeCut(pasteboard, *cut_token);
                  else
                      PasteboardSupport::ReleaseCut(pasteboard, *cut_token);
              }
              if( file_list_token ) {
                  if( fully_moved )
                      PasteboardSupport::ConsumeFileListMove(pasteboard, *file_list_token);
                  else
                      PasteboardSupport::ReleaseFileListMove(pasteboard, *file_list_token);
              }
          });
        });
        op->ObserveUnticketed(nc::ops::Operation::NotifyAboutStop,
                              [pasteboard = _pasteboard,
                               cut_token = _cut_token,
                               file_list_token = _file_list_token] {
          dispatch_to_main_queue([pasteboard, cut_token, file_list_token] {
              if( cut_token )
                  PasteboardSupport::ReleaseCut(pasteboard, *cut_token);
              if( file_list_token )
                  PasteboardSupport::ReleaseFileListMove(pasteboard, *file_list_token);
          });
        });
    }
    [window_controller enqueueOperation:op];
    return PasteSubmissionResult::Submitted;
}

PasteFromPasteboard::PasteFromPasteboard(nc::vfs::NativeHost &_native_host) : m_NativeHost(_native_host)
{
}

bool PasteFromPasteboard::Predicate(PanelController *_target) const
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    if( !_target || !_target.mainWindowController || _target.isDoingBackgroundLoading || !_target.isUniform ||
        !_target.vfs ) {
        return false;
    }
    return vfs::ProviderCapabilitiesResolver::Resolve(*_target.vfs, _target.currentDirectoryPath).can_write &&
           PasteboardSupport::CanReadFileList(pasteboard) && !PasteboardSupport::IsCutInFlight(pasteboard) &&
           !PasteboardSupport::IsFileListMoveInFlight(pasteboard);
#pragma clang diagnostic pop
}

void PasteFromPasteboard::Perform(PanelController *_target, [[maybe_unused]] id _sender) const
{
    if( Execute(_target) != PasteSubmissionResult::Submitted )
        NSBeep();
}

PasteSubmissionResult PasteFromPasteboard::Execute(PanelController *_target, NSPasteboard *_pasteboard) const
{
    NSPasteboard *const pasteboard = _pasteboard != nil ? _pasteboard : NSPasteboard.generalPasteboard;
    if( !PasteboardSupport::CanReadFileList(pasteboard) )
        return PasteSubmissionResult::ClipboardUnavailable;
    const auto cut_token = PasteboardSupport::CurrentCutToken(pasteboard);
    if( PasteboardSupport::IsFileListMoveInFlight(pasteboard) ||
        (cut_token && !PasteboardSupport::TryClaimCut(pasteboard, *cut_token)) ) {
        return PasteSubmissionResult::ClipboardBusy;
    }
    return PasteOrMove(_target, !cut_token, m_NativeHost, pasteboard, cut_token);
}

MoveFromPasteboard::MoveFromPasteboard(nc::vfs::NativeHost &_native_host) : m_NativeHost(_native_host)
{
}

bool MoveFromPasteboard::Predicate(PanelController *_target) const
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    return _target.isUniform && _target.vfs->IsWritable() && PasteboardSupport::CanReadFileList(pasteboard) &&
           !PasteboardSupport::IsCutInFlight(pasteboard) && !PasteboardSupport::IsFileListMoveInFlight(pasteboard);
#pragma clang diagnostic pop
}

void MoveFromPasteboard::Perform(PanelController *_target, [[maybe_unused]] id _sender) const
{
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    if( !PasteboardSupport::CanReadFileList(pasteboard) ) {
        NSBeep();
        return;
    }
    const auto cut_token = PasteboardSupport::CurrentCutToken(pasteboard);
    std::optional<PasteboardFileListToken> file_list_token;
    if( PasteboardSupport::IsFileListMoveInFlight(pasteboard) ) {
        NSBeep();
        return;
    }
    if( cut_token ) {
        if( !PasteboardSupport::TryClaimCut(pasteboard, *cut_token) ) {
            NSBeep();
            return;
        }
    }
    else {
        file_list_token = PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard);
        if( !file_list_token ) {
            NSBeep();
            return;
        }
    }
    [[maybe_unused]] const PasteSubmissionResult submitted =
        PasteOrMove(_target, false, m_NativeHost, pasteboard, cut_token, file_list_token);
}

}; // namespace nc::panel::actions
