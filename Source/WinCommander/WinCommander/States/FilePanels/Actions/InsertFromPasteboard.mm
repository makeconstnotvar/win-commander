// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include <VFS/Native.h>
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
// or even with custom NC's structures used in drag&drop system

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

static PasteboardSourceItems FetchVFSListingsItemsFromPasteboard(vfs::NativeHost &_native_host)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

    // check what's inside pasteboard
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    if( [pasteboard availableTypeFromArray:@[NSFilenamesPboardType]] ) {
        // input should be an array of filepaths as NSStrings
        auto filepaths = objc_cast<NSArray>([pasteboard propertyListForType:NSFilenamesPboardType]);

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

static void PasteOrMove(PanelController *_target,
                        bool _paste,
                        vfs::NativeHost &_native_host,
                        std::optional<PasteboardCutToken> _cut_token = std::nullopt,
                        std::optional<PasteboardFileListToken> _file_list_token = std::nullopt)
{
    const auto release_move_claim = [&] {
        if( _cut_token )
            PasteboardSupport::ReleaseCut(NSPasteboard.generalPasteboard, *_cut_token);
        if( _file_list_token )
            PasteboardSupport::ReleaseFileListMove(NSPasteboard.generalPasteboard, *_file_list_token);
    };

    // check if we're on uniform panel with a writeable VFS
    if( !_target.isUniform || !_target.vfs->IsWritable() ) {
        release_move_claim();
        return;
    }

    auto pasteboard_items = FetchVFSListingsItemsFromPasteboard(_native_host);

    if( pasteboard_items.items.empty() ) {
        release_move_claim();
        return; // errors on fetching listings?
    }

    if( !_paste && pasteboard_items.items.size() != pasteboard_items.listed_paths ) {
        release_move_claim();
        return;
    }

    // Revalidate after reading the paths. An external clipboard replacement must never turn
    // a newly supplied file list into a destructive move.
    if( !_paste && _cut_token ) {
        const auto current_token = PasteboardSupport::CurrentCutToken(NSPasteboard.generalPasteboard);
        if( !current_token || *current_token != *_cut_token ) {
            release_move_claim();
            return;
        }
    }
    if( !_paste && _file_list_token &&
        !PasteboardSupport::IsFileListMoveClaimCurrent(NSPasteboard.generalPasteboard, *_file_list_token) ) {
        release_move_claim();
        return;
    }

    std::optional<std::vector<NativeSourceIdentity>> source_identities;
    if( !_paste ) {
        source_identities = CaptureSourceIdentities(pasteboard_items.items);
        if( !source_identities ) {
            release_move_claim();
            return;
        }
    }

    auto opts = MakeDefaultFileCopyOptions();
    opts.docopy = _paste;
    __weak PanelController *wpc = _target;
    const auto op = std::make_shared<nc::ops::Copying>(
        std::move(pasteboard_items.items), _target.currentDirectoryPath, _target.vfs, opts);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [=] {
        dispatch_to_main_queue([=] {
            if( PanelController *const pc = wpc )
                [pc refreshPanel];
        });
    });
    if( !_paste ) {
        op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion,
                              [cut_token = _cut_token,
                               file_list_token = _file_list_token,
                               identities = std::move(*source_identities)] {
          const bool fully_moved = OriginalSourceItemsWereMoved(identities);
          dispatch_to_main_queue([cut_token, file_list_token, fully_moved] {
              if( cut_token ) {
                  if( fully_moved )
                      PasteboardSupport::ConsumeCut(NSPasteboard.generalPasteboard, *cut_token);
                  else
                      PasteboardSupport::ReleaseCut(NSPasteboard.generalPasteboard, *cut_token);
              }
              if( file_list_token ) {
                  if( fully_moved )
                      PasteboardSupport::ConsumeFileListMove(NSPasteboard.generalPasteboard, *file_list_token);
                  else
                      PasteboardSupport::ReleaseFileListMove(NSPasteboard.generalPasteboard, *file_list_token);
              }
          });
        });
        op->ObserveUnticketed(nc::ops::Operation::NotifyAboutStop,
                              [cut_token = _cut_token, file_list_token = _file_list_token] {
          dispatch_to_main_queue([cut_token, file_list_token] {
              if( cut_token )
                  PasteboardSupport::ReleaseCut(NSPasteboard.generalPasteboard, *cut_token);
              if( file_list_token )
                  PasteboardSupport::ReleaseFileListMove(NSPasteboard.generalPasteboard, *file_list_token);
          });
        });
    }
    [_target.mainWindowController enqueueOperation:op];
}

PasteFromPasteboard::PasteFromPasteboard(nc::vfs::NativeHost &_native_host) : m_NativeHost(_native_host)
{
}

bool PasteFromPasteboard::Predicate(PanelController *_target) const
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    return _target.isUniform && _target.vfs->IsWritable() && PasteboardSupport::CanReadFileList(pasteboard) &&
           !PasteboardSupport::IsCutInFlight(pasteboard) && !PasteboardSupport::IsFileListMoveInFlight(pasteboard);
#pragma clang diagnostic pop
}

void PasteFromPasteboard::Perform(PanelController *_target, [[maybe_unused]] id _sender) const
{
    NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
    if( !PasteboardSupport::CanReadFileList(pasteboard) ) {
        NSBeep();
        return;
    }
    const auto cut_token = PasteboardSupport::CurrentCutToken(pasteboard);
    if( PasteboardSupport::IsFileListMoveInFlight(pasteboard) ||
        (cut_token && !PasteboardSupport::TryClaimCut(pasteboard, *cut_token)) ) {
        NSBeep();
        return;
    }
    PasteOrMove(_target, !cut_token, m_NativeHost, cut_token);
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
    PasteOrMove(_target, false, m_NativeHost, cut_token, file_list_token);
}

}; // namespace nc::panel::actions
