// Copyright (C) 2017-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "MakeNew.h"
#include "../PanelController.h"
#include "../MainWindowFilePanelState.h"
#include "../PanelAux.h"
#include <Panel/PanelData.h>
#include "../PanelView.h"
#include "../../MainWindowController.h"
#include <Operations/DirectoryCreation.h>
#include <Operations/DirectoryCreationDialog.h>
#include <Operations/EmptyFileCreation.h>
#include <Operations/Copying.h>
#include <VFS/Native.h>
#include <VFS/NetSFTP.h>
#include <VFS/ProviderCapabilities.h>
#include <Utility/StringExtras.h>
#include <Base/dispatch_cpp.h>
#include <algorithm>
#include <expected>

namespace nc::panel::actions {

using namespace std::literals;

[[clang::no_destroy]] static const auto g_InitialFileName = []() -> std::string {
    NSString *const stub = NSLocalizedString(@"untitled.txt", "Name for freshly created file by hotkey");
    if( stub && stub.length )
        return stub.fileSystemRepresentationSafe;

    return "untitled.txt";
}();

[[clang::no_destroy]] static const auto g_InitialFolderName = []() -> std::string {
    NSString *const stub = NSLocalizedString(@"untitled folder", "Name for freshly create folder by hotkey");
    if( stub && stub.length )
        return stub.fileSystemRepresentationSafe;

    return "untitled folder";
}();

[[clang::no_destroy]] static const auto g_InitialFolderWithItemsName = []() -> std::string {
    NSString *const stub =
        NSLocalizedString(@"New Folder with Items", "Name for freshly created folder by hotkey with items");
    if( stub && stub.length )
        return stub.fileSystemRepresentationSafe;

    return "New Folder with Items";
}();

static std::string NextName(const std::string &_initial, int _index)
{
    std::filesystem::path p = _initial;
    if( p.has_extension() ) {
        auto ext = p.extension();
        p.replace_extension();
        return p.native() + " " + std::to_string(_index) + ext.native();
    }
    else
        return p.native() + " " + std::to_string(_index);
}

static bool HasEntry(const std::string &_name, const VFSListing &_listing, bool _case_sensitive)
{
    // naive O(n) implementation, may cause troubles on huge listings
    const unsigned size = _listing.Count();
    if( _case_sensitive ) {
        for( unsigned i = 0; i != size; ++i ) {
            if( _listing.Filename(i) == _name )
                return true;
        }
    }
    else {
        auto name = [NSString stringWithUTF8StdString:_name];
        for( unsigned i = 0; i != size; ++i ) {
            if( [name compare:_listing.FilenameNS(i) options:NSCaseInsensitiveSearch] == NSOrderedSame )
                return true;
        }
    }
    return false;
}

static std::string FindSuitableName(const std::string &_initial, const VFSListing &_listing, bool _case_sensitive)
{
    auto name = _initial;
    if( !HasEntry(name, _listing, _case_sensitive) )
        return name;

    for( int i = 2;; ++i ) {
        name = NextName(_initial, i);
        if( !HasEntry(name, _listing, _case_sensitive) )
            break;
        if( i >= 100 )
            return ""; // we're full of such filenames, no reason to go on
    }
    return name;
}

static bool IsValidQuickName(const std::string_view _name)
{
    if( _name.empty() || _name.size() > 255 || _name == "." || _name == ".." )
        return false;
    return std::ranges::none_of(_name, [](const unsigned char c) { return c == '/' || c == '\0' || c < 0x20; });
}

struct QuickNewFilePreparation final {
    NCMainWindowController *__strong window_controller;
    VFSHostPtr vfs;
    VFSListingPtr listing;
    std::filesystem::path directory;
    std::string name;
};

[[nodiscard]] static std::expected<QuickNewFilePreparation, QuickNewFileSubmissionResult>
PrepareQuickNewFile(PanelController *_target)
{
    if( !_target )
        return std::unexpected(QuickNewFileSubmissionResult::PaneUnavailable);
    NCMainWindowController *const window_controller = _target.mainWindowController;
    if( !window_controller )
        return std::unexpected(QuickNewFileSubmissionResult::WindowUnavailable);
    if( _target.isDoingBackgroundLoading )
        return std::unexpected(QuickNewFileSubmissionResult::Loading);
    if( !_target.isUniform || !_target.vfs || !_target.data.ListingPtr() )
        return std::unexpected(QuickNewFileSubmissionResult::DestinationUnavailable);

    const std::filesystem::path directory = _target.currentDirectoryPath;
    const VFSHostPtr vfs = _target.vfs;
    const VFSListingPtr listing = _target.data.ListingPtr();
    try {
        if( !vfs->IsWritableAtPath(directory.native()) )
            return std::unexpected(QuickNewFileSubmissionResult::DestinationReadOnly);
        if( !vfs::ProviderCapabilitiesResolver::Resolve(*vfs, directory.native()).can_create_file ||
            !SupportsExclusiveQuickNewFile(*vfs) ) {
            return std::unexpected(QuickNewFileSubmissionResult::ProviderUnsupported);
        }
        std::string name =
            FindSuitableName(g_InitialFileName, *listing, vfs->IsCaseSensitiveAtPath(directory.native()));
        if( !IsValidQuickName(name) )
            return std::unexpected(QuickNewFileSubmissionResult::NameUnavailable);
        return QuickNewFilePreparation{
            .window_controller = window_controller,
            .vfs = vfs,
            .listing = listing,
            .directory = directory,
            .name = std::move(name),
        };
    } catch( ... ) {
        return std::unexpected(QuickNewFileSubmissionResult::DestinationUnavailable);
    }
}

bool SupportsExclusiveQuickNewFile(const nc::vfs::Host &_host) noexcept
{
    const std::string_view tag = _host.Tag();
    return tag == VFSNativeHost::UniqueTag || tag == vfs::SFTPHost::UniqueTag;
}

static void ScheduleRenaming(const std::string &_filename, PanelController *_panel)
{
    __weak PanelController *weak_panel = _panel;
    DelayedFocusing req;
    req.filename = _filename;
    req.timeout = 2s;
    req.done = [=] {
        [static_cast<PanelController *>(weak_panel).view discardFieldEditor];
        [static_cast<PanelController *>(weak_panel).view startFieldEditorRenaming];
    };
    [_panel scheduleDelayedFocusing:req];
}

static void ScheduleFocus(const std::string &_filename, PanelController *_panel)
{
    DelayedFocusing req;
    req.filename = _filename;
    req.timeout = 2s;
    [_panel scheduleDelayedFocusing:req];
}

bool MakeNewFile::Predicate(PanelController *_target) const
{
    return EvaluateQuickNewFileSubmission(_target) == QuickNewFileSubmissionResult::Submitted;
}

void MakeNewFile::Perform(PanelController *_target, id /*_sender*/) const
{
    if( SubmitQuickNewFile(_target) != QuickNewFileSubmissionResult::Submitted )
        NSBeep();
}

QuickNewFileSubmissionResult EvaluateQuickNewFileSubmission(PanelController *_target)
{
    const auto prepared = PrepareQuickNewFile(_target);
    return prepared ? QuickNewFileSubmissionResult::Submitted : prepared.error();
}

QuickNewFileSubmissionResult SubmitQuickNewFile(PanelController *_target)
{
    auto prepared = PrepareQuickNewFile(_target);
    if( !prepared )
        return prepared.error();

    NCMainWindowController *const window_controller = prepared->window_controller;
    const std::filesystem::path dir = std::move(prepared->directory);
    const VFSHostPtr vfs = std::move(prepared->vfs);
    const VFSListingPtr listing = std::move(prepared->listing);
    const std::string name = std::move(prepared->name);

    try {
        if( _target.mainWindowController != window_controller || _target.isDoingBackgroundLoading ||
            !_target.isUniform || _target.vfs != vfs || _target.currentDirectoryPath != dir.native() ||
            _target.data.ListingPtr() != listing || !vfs->IsWritableAtPath(dir.native()) ||
            !vfs::ProviderCapabilitiesResolver::Resolve(*vfs, dir.native()).can_create_file ||
            !SupportsExclusiveQuickNewFile(*vfs) ) {
            return QuickNewFileSubmissionResult::StaleDestination;
        }
    } catch( ... ) {
        return QuickNewFileSubmissionResult::StaleDestination;
    }

    __weak PanelController *weak_panel = _target;
    const auto operation = std::make_shared<nc::ops::EmptyFileCreation>(name, dir.native(), *vfs);
    operation->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [=] {
        dispatch_to_main_queue([=] {
            PanelController *const panel = weak_panel;
            if( panel && panel.vfs == vfs && panel.currentDirectoryPath == dir.native() ) {
                [panel hintAboutFilesystemChange];
                ScheduleRenaming(name, panel);
            }
        });
    });
    [window_controller enqueueOperation:operation];
    return QuickNewFileSubmissionResult::Submitted;
}

bool MakeNewFolder::Predicate(PanelController *_target) const
{
    if( !_target || !_target.mainWindowController || _target.isDoingBackgroundLoading || !_target.isUniform ||
        !_target.vfs || !_target.data.ListingPtr() ) {
        return false;
    }
    const std::string path = _target.currentDirectoryPath;
    return _target.vfs->IsWritableAtPath(path) &&
           vfs::ProviderCapabilitiesResolver::Resolve(*_target.vfs, path).can_create_folder;
}

void MakeNewFolder::Perform(PanelController *_target, id /*_sender*/) const
{
    if( SubmitQuickNewFolder(_target) != QuickNewFolderSubmissionResult::Submitted )
        NSBeep();
}

QuickNewFolderSubmissionResult SubmitQuickNewFolder(PanelController *_target)
{
    if( !_target )
        return QuickNewFolderSubmissionResult::PaneUnavailable;
    auto *const window_controller = _target.mainWindowController;
    if( !window_controller )
        return QuickNewFolderSubmissionResult::WindowUnavailable;
    if( _target.isDoingBackgroundLoading )
        return QuickNewFolderSubmissionResult::Loading;
    if( !_target.isUniform || !_target.vfs || !_target.data.ListingPtr() )
        return QuickNewFolderSubmissionResult::DestinationUnavailable;

    const std::filesystem::path dir = _target.currentDirectoryPath;
    const VFSHostPtr vfs = _target.vfs;
    const VFSListingPtr listing = _target.data.ListingPtr();
    bool case_sensitive = true;
    try {
        if( !vfs->IsWritableAtPath(dir.native()) )
            return QuickNewFolderSubmissionResult::DestinationReadOnly;
        if( !vfs::ProviderCapabilitiesResolver::Resolve(*vfs, dir.native()).can_create_folder )
            return QuickNewFolderSubmissionResult::ProviderUnsupported;
        case_sensitive = vfs->IsCaseSensitiveAtPath(dir.native());
    } catch( ... ) {
        return QuickNewFolderSubmissionResult::DestinationUnavailable;
    }
    __weak PanelController *weak_panel = _target;

    const auto name = FindSuitableName(g_InitialFolderName, *listing, case_sensitive);
    if( !IsValidQuickName(name) )
        return QuickNewFolderSubmissionResult::NameUnavailable;

    try {
        if( _target.mainWindowController != window_controller || _target.isDoingBackgroundLoading ||
            !_target.isUniform || _target.vfs != vfs || _target.currentDirectoryPath != dir.native() ||
            _target.data.ListingPtr() != listing || !vfs->IsWritableAtPath(dir.native()) ||
            !vfs::ProviderCapabilitiesResolver::Resolve(*vfs, dir.native()).can_create_folder ) {
            return QuickNewFolderSubmissionResult::StaleDestination;
        }
    } catch( ... ) {
        return QuickNewFolderSubmissionResult::StaleDestination;
    }

    const auto op = std::make_shared<nc::ops::DirectoryCreation>(name, dir.native(), *vfs);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [=] {
        dispatch_to_main_queue([=] {
            if( PanelController *const panel = weak_panel ) {
                [panel hintAboutFilesystemChange];
                ScheduleRenaming(name, panel);
            }
        });
    });

    [window_controller enqueueOperation:op];
    return QuickNewFolderSubmissionResult::Submitted;
}

bool MakeNewFolderWithSelection::Predicate(PanelController *_target) const
{
    auto item = _target.view.item;
    return _target.isUniform && _target.vfs->IsWritable() && item &&
           (!item.IsDotDot() || _target.data.Stats().selected_entries_amount > 0);
}

void MakeNewFolderWithSelection::Perform(PanelController *_target, id /*_sender*/) const
{
    const std::filesystem::path dir = _target.currentDirectoryPath;
    const VFSHostPtr vfs = _target.vfs;
    const VFSListingPtr listing = _target.data.ListingPtr();
    const bool case_sensitive = vfs->IsCaseSensitiveAtPath(dir.c_str());
    __weak PanelController *weak_panel = _target;
    const auto files = _target.selectedEntriesOrFocusedEntry;

    if( files.empty() )
        return;

    const auto name = FindSuitableName(g_InitialFolderWithItemsName, *listing, case_sensitive);
    if( name.empty() )
        return;

    const std::filesystem::path destination = (dir / name).concat("/");

    const auto options = MakeDefaultFileMoveOptions();
    const auto op = std::make_shared<nc::ops::Copying>(files, destination.native(), vfs, options);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [=] {
        dispatch_to_main_queue([=] {
            if( PanelController *const panel = weak_panel ) {
                [panel hintAboutFilesystemChange];
                ScheduleRenaming(name, panel);
            }
        });
    });

    [_target.mainWindowController enqueueOperation:op];
}

bool MakeNewNamedFolder::Predicate(PanelController *_target) const
{
    return _target.isUniform && _target.vfs->IsWritable();
}

static bool ValidateDirectoryInput(const std::string &_text)
{
    const auto max_len = 256;
    if( _text.empty() || _text.length() > max_len )
        return false;
    static const auto invalid_chars = ":\\\r\t\n";
    return _text.find_first_of(invalid_chars) == std::string::npos;
}

void MakeNewNamedFolder::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto cd = [[NCOpsDirectoryCreationDialog alloc] init];
    if( const auto item = _target.view.item )
        if( !item.IsDotDot() )
            cd.suggestion = item.Filename();

    cd.validationCallback = ValidateDirectoryInput;

    auto handler = ^(NSModalResponse returnCode) {
      if( returnCode == NSModalResponseOK && !cd.result.empty() ) {
          const std::string name = cd.result;
          const std::string dir = _target.currentDirectoryPath;
          const auto vfs = _target.vfs;
          __weak PanelController *weak_panel = _target;

          const auto op = std::make_shared<nc::ops::DirectoryCreation>(name, dir, *vfs);
          const auto weak_op = std::weak_ptr<nc::ops::DirectoryCreation>{op};
          op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [=] {
              const auto &dir_names = weak_op.lock()->DirectoryNames();
              const std::string to_focus = dir_names.empty() ? ""s : dir_names.front();
              dispatch_to_main_queue([=] {
                  if( PanelController *const panel = weak_panel ) {
                      [panel hintAboutFilesystemChange];
                      ScheduleFocus(to_focus, panel);
                  }
              });
          });

          [_target.mainWindowController enqueueOperation:op];
      }
    };
    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

static PanelController *FindOppositeController(PanelController *_source)
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

bool MakeNewNamedFolderInOppositePanel::Predicate(PanelController *_target) const
{
    PanelController *const opposite = FindOppositeController(_target);
    if( !opposite )
        return false;
    return opposite.isUniform && opposite.vfs->IsWritable();
}

void MakeNewNamedFolderInOppositePanel::Perform(PanelController *_target, id /*_sender*/) const
{
    PanelController *const opposite = FindOppositeController(_target);
    if( !opposite || !opposite.isUniform || !opposite.vfs->IsWritable() )
        return;

    const auto cd = [[NCOpsDirectoryCreationDialog alloc] init];
    if( const auto item = _target.view.item )
        if( !item.IsDotDot() )
            cd.suggestion = item.Filename();

    cd.validationCallback = ValidateDirectoryInput;

    auto handler = ^(NSModalResponse returnCode) {
      if( returnCode == NSModalResponseOK && !cd.result.empty() ) {
          const std::string name = cd.result;
          const std::string dir = opposite.currentDirectoryPath;
          const auto vfs = opposite.vfs;
          __weak PanelController *weak_opposite = opposite;

          const auto op = std::make_shared<nc::ops::DirectoryCreation>(name, dir, *vfs);
          const auto weak_op = std::weak_ptr<nc::ops::DirectoryCreation>{op};
          op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [=] {
              const auto &dir_names = weak_op.lock()->DirectoryNames();
              const std::string to_focus = dir_names.empty() ? ""s : dir_names.front();
              dispatch_to_main_queue([=] {
                  if( PanelController *const panel = weak_opposite ) {
                      [panel hintAboutFilesystemChange];
                      ScheduleFocus(to_focus, panel);
                  }
              });
          });

          [_target.mainWindowController enqueueOperation:op];
      }
    };
    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

} // namespace nc::panel::actions
