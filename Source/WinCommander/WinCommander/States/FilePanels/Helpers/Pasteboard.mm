// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Pasteboard.h"
#include <VFS/VFS.h>
#include <Utility/StringExtras.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace nc::panel {

NSNotificationName const NCPanelPasteboardCutStateDidChangeNotification =
    @"NCPanelPasteboardCutStateDidChangeNotification";

static NSPasteboardType const g_MoveFileListPasteboardType = @"com.wincommander.file-list.move";

struct CutSnapshot {
    PasteboardCutToken token;
    std::vector<std::string> paths;
    std::unordered_set<std::string> path_set;
    bool in_flight = false;
};

struct FileListMoveReservation {
    PasteboardFileListToken token;
    std::vector<std::string> paths;
};

[[clang::no_destroy]] static std::mutex g_CutSnapshotLock;
[[clang::no_destroy]] static std::optional<CutSnapshot> g_CutSnapshot;
[[clang::no_destroy]] static std::unordered_map<std::string, FileListMoveReservation> g_FileListMoveReservations;
[[clang::no_destroy]] static std::unordered_map<std::string, NSInteger> g_ConsumedFileListGenerations;

static std::string PasteboardName(NSPasteboard *_pasteboard)
{
    if( !_pasteboard )
        return {};
    return _pasteboard.name.UTF8String ? _pasteboard.name.UTF8String : "";
}

static std::optional<std::vector<std::string>> FilePathsFromPasteboard(NSPasteboard *_pasteboard)
{
    if( !_pasteboard )
        return std::nullopt;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    id const property_list = [_pasteboard propertyListForType:NSFilenamesPboardType];
#pragma clang diagnostic pop
    if( ![property_list isKindOfClass:NSArray.class] )
        return std::nullopt;

    std::vector<std::string> paths;
    paths.reserve([static_cast<NSArray *>(property_list) count]);
    for( id const value in static_cast<NSArray *>(property_list) ) {
        if( ![value isKindOfClass:NSString.class] )
            return std::nullopt;
        NSString *const path = static_cast<NSString *>(value);
        if( path.length == 0 || !path.fileSystemRepresentation )
            return std::nullopt;
        paths.emplace_back(path.fileSystemRepresentation);
    }
    return paths.empty() ? std::nullopt : std::optional{std::move(paths)};
}

static bool InvalidateCutSnapshotForPasteboard(
    NSPasteboard *_pasteboard,
    const std::optional<PasteboardCutToken> &_expected_token = std::nullopt)
{
    const std::string pasteboard_name = PasteboardName(_pasteboard);
    bool invalidated = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( g_CutSnapshot && g_CutSnapshot->token.pasteboard_name == pasteboard_name &&
            (!_expected_token || g_CutSnapshot->token == *_expected_token) ) {
            g_CutSnapshot.reset();
            invalidated = true;
        }
    }
    if( invalidated )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return invalidated;
}

bool PasteboardSupport::WriteFilesnamesPBoard(const std::vector<VFSListingItem> &_items,
                                              NSPasteboard *_pasteboard,
                                              PasteboardFileOperation _operation)
{
    if( !_pasteboard )
        return false;

    auto filepaths = [[NSMutableArray alloc] initWithCapacity:_items.size()];
    for( auto &i : _items )
        if( i.Host() && i.Host()->IsNativeFS() )
            if( auto path = [NSString stringWithUTF8StdString:i.Path()] )
                [filepaths addObject:path];

    if( filepaths.count == 0 )
        return false;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [_pasteboard clearContents];
    [_pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];

    const bool wrote_files = [_pasteboard setPropertyList:filepaths forType:NSFilenamesPboardType];
    if( !wrote_files ) {
        InvalidateCutSnapshotForPasteboard(_pasteboard);
        return false;
    }
    if( _operation == PasteboardFileOperation::Move )
        return MarkCurrentFileListForMove(_pasteboard);
    InvalidateCutSnapshotForPasteboard(_pasteboard);
    return true;
#pragma clang diagnostic pop
}

bool PasteboardSupport::CanReadFileList(NSPasteboard *_pasteboard)
{
    // Reconcile a previously owned Cut whenever a clipboard consumer observes the pasteboard.
    // This also removes the visual Cut state promptly after another application replaces it.
    (void)CurrentCutToken(_pasteboard);

    const std::string pasteboard_name = PasteboardName(_pasteboard);
    const NSInteger change_count = _pasteboard ? _pasteboard.changeCount : -1;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( const auto consumed = g_ConsumedFileListGenerations.find(pasteboard_name);
            consumed != g_ConsumedFileListGenerations.end() ) {
            if( consumed->second == change_count )
                return false;
            g_ConsumedFileListGenerations.erase(consumed);
        }
    }
    return FilePathsFromPasteboard(_pasteboard).has_value();
}

bool PasteboardSupport::MarkCurrentFileListForMove(NSPasteboard *_pasteboard)
{
    if( !_pasteboard )
        return false;
    const auto paths = FilePathsFromPasteboard(_pasteboard);
    if( !paths ) {
        InvalidateCutSnapshotForPasteboard(_pasteboard);
        return false;
    }

    const std::string nonce = NSUUID.UUID.UUIDString.UTF8String;
    NSData *const marker = [NSData dataWithBytes:nonce.data() length:nonce.size()];
    [_pasteboard addTypes:@[g_MoveFileListPasteboardType] owner:nil];
    if( ![_pasteboard setData:marker forType:g_MoveFileListPasteboardType] ) {
        InvalidateCutSnapshotForPasteboard(_pasteboard);
        return false;
    }

    CutSnapshot snapshot;
    snapshot.token.pasteboard_name = PasteboardName(_pasteboard);
    snapshot.token.change_count = _pasteboard.changeCount;
    snapshot.token.nonce = nonce;
    snapshot.paths = *paths;
    snapshot.path_set.insert(paths->begin(), paths->end());

    {
        const std::lock_guard lock(g_CutSnapshotLock);
        g_CutSnapshot = std::move(snapshot);
    }
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                      object:_pasteboard];
    return true;
}

std::optional<PasteboardCutToken> PasteboardSupport::CurrentCutToken(NSPasteboard *_pasteboard)
{
    if( !_pasteboard )
        return std::nullopt;

    PasteboardCutToken token;
    std::vector<std::string> expected_paths;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( !g_CutSnapshot )
            return std::nullopt;
        token = g_CutSnapshot->token;
        expected_paths = g_CutSnapshot->paths;
    }
    if( token.pasteboard_name != PasteboardName(_pasteboard) )
        return std::nullopt;

    const auto paths = FilePathsFromPasteboard(_pasteboard);
    NSData *const marker = [_pasteboard dataForType:g_MoveFileListPasteboardType];
    const std::string nonce = marker.length > 0
                                  ? std::string(static_cast<const char *>(marker.bytes), marker.length)
                                  : "";
    const bool matches = token.change_count == _pasteboard.changeCount && token.nonce == nonce && paths &&
                         expected_paths == *paths;
    if( !matches ) {
        InvalidateCutSnapshotForPasteboard(_pasteboard, token);
        return std::nullopt;
    }

    const std::lock_guard lock(g_CutSnapshotLock);
    return g_CutSnapshot && g_CutSnapshot->token == token ? std::optional{token} : std::nullopt;
}

bool PasteboardSupport::TryClaimCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token)
{
    const auto current = CurrentCutToken(_pasteboard);
    if( !current || *current != _token )
        return false;

    bool claimed = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( g_CutSnapshot && g_CutSnapshot->token == _token && !g_CutSnapshot->in_flight &&
            _pasteboard.changeCount == _token.change_count ) {
            g_CutSnapshot->in_flight = true;
            claimed = true;
        }
    }
    if( claimed )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return claimed;
}

bool PasteboardSupport::ReleaseCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token)
{
    bool released = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( g_CutSnapshot && g_CutSnapshot->token == _token && g_CutSnapshot->in_flight ) {
            g_CutSnapshot->in_flight = false;
            released = true;
        }
    }
    if( released )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return released;
}

bool PasteboardSupport::IsCutInFlight(NSPasteboard *_pasteboard)
{
    const auto current = CurrentCutToken(_pasteboard);
    if( !current )
        return false;

    const std::lock_guard lock(g_CutSnapshotLock);
    return g_CutSnapshot && g_CutSnapshot->token == *current && g_CutSnapshot->in_flight;
}

bool PasteboardSupport::ConsumeCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token)
{
    const auto current = CurrentCutToken(_pasteboard);
    if( !current || *current != _token )
        return false;

    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( !g_CutSnapshot || g_CutSnapshot->token != _token || !g_CutSnapshot->in_flight )
            return false;
        g_ConsumedFileListGenerations[_token.pasteboard_name] = _token.change_count;
        g_CutSnapshot.reset();
    }
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                      object:_pasteboard];
    return true;
}

std::optional<PasteboardFileListToken>
PasteboardSupport::TryClaimCurrentFileListForMove(NSPasteboard *_pasteboard)
{
    const auto paths = FilePathsFromPasteboard(_pasteboard);
    if( !paths )
        return std::nullopt;

    PasteboardFileListToken token;
    token.pasteboard_name = PasteboardName(_pasteboard);
    token.change_count = _pasteboard.changeCount;
    token.nonce = NSUUID.UUID.UUIDString.UTF8String;

    bool claimed = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        const auto consumed = g_ConsumedFileListGenerations.find(token.pasteboard_name);
        const bool generation_was_consumed = consumed != g_ConsumedFileListGenerations.end() &&
                                             consumed->second == token.change_count;
        if( consumed != g_ConsumedFileListGenerations.end() && !generation_was_consumed )
            g_ConsumedFileListGenerations.erase(consumed);
        if( !generation_was_consumed && _pasteboard.changeCount == token.change_count &&
            !g_FileListMoveReservations.contains(token.pasteboard_name) ) {
            g_FileListMoveReservations.emplace(token.pasteboard_name,
                                               FileListMoveReservation{.token = token, .paths = *paths});
            claimed = true;
        }
    }
    if( claimed )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return claimed ? std::optional{token} : std::nullopt;
}

bool PasteboardSupport::IsFileListMoveClaimCurrent(NSPasteboard *_pasteboard,
                                                    const PasteboardFileListToken &_token)
{
    if( !_pasteboard || PasteboardName(_pasteboard) != _token.pasteboard_name ||
        _pasteboard.changeCount != _token.change_count )
        return false;

    const auto paths = FilePathsFromPasteboard(_pasteboard);
    if( !paths )
        return false;

    const std::lock_guard lock(g_CutSnapshotLock);
    const auto reservation = g_FileListMoveReservations.find(_token.pasteboard_name);
    return reservation != g_FileListMoveReservations.end() && reservation->second.token == _token &&
           reservation->second.paths == *paths;
}

bool PasteboardSupport::IsFileListMoveInFlight(NSPasteboard *_pasteboard)
{
    const std::string pasteboard_name = PasteboardName(_pasteboard);
    const std::lock_guard lock(g_CutSnapshotLock);
    return g_FileListMoveReservations.contains(pasteboard_name);
}

bool PasteboardSupport::ReleaseFileListMove(NSPasteboard *_pasteboard, const PasteboardFileListToken &_token)
{
    bool released = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        const auto reservation = g_FileListMoveReservations.find(_token.pasteboard_name);
        if( reservation != g_FileListMoveReservations.end() && reservation->second.token == _token ) {
            g_FileListMoveReservations.erase(reservation);
            released = true;
        }
    }
    if( released )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return released;
}

bool PasteboardSupport::ConsumeFileListMove(NSPasteboard *_pasteboard, const PasteboardFileListToken &_token)
{
    bool consumed = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        const auto reservation = g_FileListMoveReservations.find(_token.pasteboard_name);
        if( reservation != g_FileListMoveReservations.end() && reservation->second.token == _token ) {
            g_ConsumedFileListGenerations[_token.pasteboard_name] = _token.change_count;
            g_FileListMoveReservations.erase(reservation);
            consumed = true;
        }
    }
    if( consumed )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return consumed;
}

bool PasteboardSupport::CancelCut(NSPasteboard *_pasteboard)
{
    const auto current = CurrentCutToken(_pasteboard);
    if( !current )
        return false;

    bool cancelled = false;
    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( g_CutSnapshot && g_CutSnapshot->token == *current && !g_CutSnapshot->in_flight ) {
            g_CutSnapshot.reset();
            cancelled = true;
        }
    }
    if( cancelled )
        [NSNotificationCenter.defaultCenter postNotificationName:NCPanelPasteboardCutStateDidChangeNotification
                                                          object:_pasteboard];
    return cancelled;
}

bool PasteboardSupport::IsCutItem(NSPasteboard *_pasteboard, const std::string &_path)
{
    if( !_pasteboard )
        return false;

    {
        const std::lock_guard lock(g_CutSnapshotLock);
        if( g_CutSnapshot && g_CutSnapshot->token.pasteboard_name == PasteboardName(_pasteboard) &&
            g_CutSnapshot->token.change_count == _pasteboard.changeCount )
            return g_CutSnapshot->path_set.contains(_path);
    }

    // Invalidate a stale snapshot once, then subsequent row lookups stay O(1).
    (void)CurrentCutToken(_pasteboard);
    return false;
}

PasteboardFileOperation PasteboardSupport::FileOperation(NSPasteboard *_pasteboard)
{
    return CurrentCutToken(_pasteboard) ? PasteboardFileOperation::Move : PasteboardFileOperation::Copy;
}

bool PasteboardSupport::WriteURLSPBoard(const std::vector<VFSListingItem> &_items, NSPasteboard *_pasteboard)
{
    if( !_pasteboard )
        return false;

    auto urls = [[NSMutableArray alloc] initWithCapacity:_items.size()];
    for( auto &i : _items )
        if( i.Host() && i.Host()->IsNativeFS() )
            if( auto path = [NSString stringWithUTF8StdString:i.Path()] )
                if( auto url = [NSURL fileURLWithPath:path] )
                    [urls addObject:url];

    [_pasteboard clearContents];
    [_pasteboard declareTypes:@[(__bridge NSString *)kUTTypeFileURL] owner:nil];
    const bool wrote_urls = [_pasteboard writeObjects:urls];
    InvalidateCutSnapshotForPasteboard(_pasteboard);
    return wrote_urls;
}

} // namespace nc::panel
