// Copyright (C) 2017-2018 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <VFS/VFS.h>
#include <optional>
#include <string>

namespace nc::panel {

extern NSNotificationName const NCPanelPasteboardCutStateDidChangeNotification;

enum class PasteboardFileOperation : unsigned char {
    Copy,
    Move,
};

struct PasteboardCutToken {
    std::string pasteboard_name;
    NSInteger change_count = -1;
    std::string nonce;

    bool operator==(const PasteboardCutToken &) const noexcept = default;
};

struct PasteboardFileListToken {
    std::string pasteboard_name;
    NSInteger change_count = -1;
    std::string nonce;

    bool operator==(const PasteboardFileListToken &) const noexcept = default;
};

struct PasteboardSupport {

    static bool WriteFilesnamesPBoard(const std::vector<VFSListingItem> &_items,
                                      NSPasteboard *_pasteboard,
                                      PasteboardFileOperation _operation = PasteboardFileOperation::Copy);
    static bool WriteURLSPBoard(const std::vector<VFSListingItem> &_items, NSPasteboard *_pasteboard);
    [[nodiscard]] static bool CanReadFileList(NSPasteboard *_pasteboard);
    static bool MarkCurrentFileListForMove(NSPasteboard *_pasteboard);
    [[nodiscard]] static std::optional<PasteboardCutToken> CurrentCutToken(NSPasteboard *_pasteboard);
    static bool TryClaimCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token);
    static bool ReleaseCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token);
    [[nodiscard]] static bool IsCutInFlight(NSPasteboard *_pasteboard);
    static bool ConsumeCut(NSPasteboard *_pasteboard, const PasteboardCutToken &_token);
    [[nodiscard]] static std::optional<PasteboardFileListToken>
    TryClaimCurrentFileListForMove(NSPasteboard *_pasteboard);
    [[nodiscard]] static bool IsFileListMoveClaimCurrent(NSPasteboard *_pasteboard,
                                                         const PasteboardFileListToken &_token);
    [[nodiscard]] static bool IsFileListMoveInFlight(NSPasteboard *_pasteboard);
    static bool ReleaseFileListMove(NSPasteboard *_pasteboard, const PasteboardFileListToken &_token);
    static bool ConsumeFileListMove(NSPasteboard *_pasteboard, const PasteboardFileListToken &_token);
    static bool CancelCut(NSPasteboard *_pasteboard);
    [[nodiscard]] static bool IsCutItem(NSPasteboard *_pasteboard, const std::string &_path);
    [[nodiscard]] static PasteboardFileOperation FileOperation(NSPasteboard *_pasteboard);
};

} // namespace nc::panel
