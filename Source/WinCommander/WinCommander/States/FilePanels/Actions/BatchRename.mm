// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "BatchRename.h"
#include "../MainWindowFilePanelState.h"
#include "../PanelController.h"
#include <Panel/PanelData.h>
#include "../PanelView.h"
#include "../../MainWindowController.h"
#include <Operations/BatchRenaming.h>
#include <Operations/BatchRenamingDialog.h>
#include <VFS/ProviderCapabilities.h>
#include <WinCommander/Core/SimpleComboBoxPersistentDataSource.h>
#include <Base/dispatch_cpp.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace nc::panel::actions {

static const auto g_ConfigPatternsPath = "filePanel.batchRename.lastPatterns";
static const auto g_ConfigSearchesPath = "filePanel.batchRename.lastSearches";
static const auto g_ConfigReplacesPath = "filePanel.batchRename.lastReplaces";

namespace {

std::optional<std::string> FilenameKey(const std::string &_filename, const bool _case_sensitive)
{
    NSString *string = [NSString stringWithUTF8String:_filename.c_str()];
    if( !string )
        return std::nullopt;
    string = string.precomposedStringWithCanonicalMapping;
    if( !_case_sensitive )
        string = string.lowercaseString;
    const char *const utf8 = string.UTF8String;
    if( !utf8 )
        return std::nullopt;
    return std::string{utf8};
}

bool IsExactChildDestination(const std::string &_destination,
                             const std::string &_directory,
                             std::string &_filename)
{
    if( _destination.empty() || _directory.empty() )
        return false;

    const std::filesystem::path destination{_destination};
    const std::filesystem::path directory{_directory};
    _filename = destination.filename().native();
    if( _filename.empty() || _filename == "." || _filename == ".." || _filename.find('/') != std::string::npos )
        return false;

    const std::filesystem::path expected = directory / _filename;
    return destination == expected;
}

BatchRenameSubmissionResult ValidatePlan(const std::vector<std::string> &_sources,
                                         const std::vector<std::string> &_destinations,
                                         const std::vector<VFSListingItem> &_initial_items,
                                         const VFSListing &_current_listing,
                                         const std::string &_directory,
                                         const bool _case_sensitive)
{
    if( _sources.empty() || _sources.size() != _destinations.size() )
        return BatchRenameSubmissionResult::InvalidPlan;

    std::unordered_map<std::string, VFSListingItem> initial_by_path;
    initial_by_path.reserve(_initial_items.size());
    for( const VFSListingItem &item : _initial_items ) {
        if( !initial_by_path.emplace(item.Path(), item).second )
            return BatchRenameSubmissionResult::InvalidPlan;
    }

    std::unordered_set<std::string> planned_sources;
    planned_sources.reserve(_sources.size());
    std::unordered_set<std::string> destination_keys;
    destination_keys.reserve(_destinations.size());
    std::vector<std::string> source_keys;
    source_keys.reserve(_sources.size());

    for( size_t index = 0; index < _sources.size(); ++index ) {
        const std::string &source = _sources[index];
        const auto initial = initial_by_path.find(source);
        if( initial == initial_by_path.end() || !planned_sources.emplace(source).second )
            return BatchRenameSubmissionResult::InvalidPlan;

        std::string destination_filename;
        if( !IsExactChildDestination(_destinations[index], _directory, destination_filename) )
            return BatchRenameSubmissionResult::InvalidPlan;

        const auto destination_key = FilenameKey(destination_filename, _case_sensitive);
        const auto source_key = FilenameKey(initial->second.Filename(), _case_sensitive);
        if( !destination_key || !source_key )
            return BatchRenameSubmissionResult::InvalidPlan;
        if( !destination_keys.emplace(*destination_key).second )
            return BatchRenameSubmissionResult::DestinationConflict;
        source_keys.emplace_back(*source_key);
    }

    for( size_t destination_index = 0; destination_index < _destinations.size(); ++destination_index ) {
        const auto destination_key = FilenameKey(
            std::filesystem::path{_destinations[destination_index]}.filename().native(), _case_sensitive);
        if( !destination_key )
            return BatchRenameSubmissionResult::InvalidPlan;
        for( size_t source_index = 0; source_index < source_keys.size(); ++source_index ) {
            if( destination_index != source_index && *destination_key == source_keys[source_index] )
                return BatchRenameSubmissionResult::DestinationConflict;
        }
    }

    for( const VFSListingItem &item : _current_listing ) {
        if( item.IsDotDot() || planned_sources.contains(item.Path()) )
            continue;
        const auto existing_key = FilenameKey(item.Filename(), _case_sensitive);
        if( !existing_key )
            return BatchRenameSubmissionResult::InvalidPlan;
        if( destination_keys.contains(*existing_key) )
            return BatchRenameSubmissionResult::DestinationConflict;
    }
    return BatchRenameSubmissionResult::Presented;
}

} // namespace

BatchRenameSubmissionResult EvaluateBatchRenameSubmission(const std::span<const VFSListingItem> _items,
                                                          PanelController *_target)
{
    if( !_target )
        return BatchRenameSubmissionResult::PaneUnavailable;
    if( !_target.mainWindowController )
        return BatchRenameSubmissionResult::WindowUnavailable;
    if( _target.isDoingBackgroundLoading )
        return BatchRenameSubmissionResult::Loading;
    if( _items.empty() )
        return BatchRenameSubmissionResult::SelectionUnavailable;
    if( std::ranges::any_of(_items, [](const VFSListingItem &_item) { return !_item || _item.IsDotDot(); }) )
        return BatchRenameSubmissionResult::ParentEntryUnsupported;

    try {
        const VFSListingPtr listing = _target.data.ListingPtr();
        const VFSHostPtr host = _target.vfs;
        const std::string directory = _target.currentDirectoryPath;
        if( !_target.data.IsLoaded() || !_target.isUniform || !listing || !listing->IsUniform() || !host ||
            directory.empty() ) {
            return BatchRenameSubmissionResult::ListingUnavailable;
        }

        std::unordered_set<std::string> paths;
        paths.reserve(_items.size());
        for( const VFSListingItem &item : _items ) {
            if( !item.Host() || item.Host() != host )
                return BatchRenameSubmissionResult::MixedProviders;
            if( item.Listing().get() != listing.get() || item.Directory() != directory )
                return BatchRenameSubmissionResult::StaleContext;
            if( !paths.emplace(item.Path()).second )
                return BatchRenameSubmissionResult::InvalidPlan;
        }

        if( !vfs::ProviderCapabilitiesResolver::Resolve(*host, directory).can_rename )
            return BatchRenameSubmissionResult::ProviderUnsupported;
    } catch( ... ) {
        return BatchRenameSubmissionResult::ListingUnavailable;
    }
    return BatchRenameSubmissionResult::Presented;
}

BatchRenameSubmissionResult PresentBatchRename(const std::span<const VFSListingItem> _items, PanelController *_target)
{
    const BatchRenameSubmissionResult live = EvaluateBatchRenameSubmission(_items, _target);
    if( live != BatchRenameSubmissionResult::Presented )
        return live;

    const std::vector<VFSListingItem> entries{_items.begin(), _items.end()};
    const VFSListingPtr listing = _target.data.ListingPtr();
    const unsigned long data_generation = _target.dataGeneration;
    const VFSHostPtr host = _target.vfs;
    const std::string directory = _target.currentDirectoryPath;
    NCMainWindowController *const window_controller = _target.mainWindowController;

    const auto sheet = [[NCOpsBatchRenamingDialog alloc] initWithItems:entries];
    sheet.renamePatternDataSource =
        [[SimpleComboBoxPersistentDataSource alloc] initWithStateConfigPath:g_ConfigPatternsPath];
    sheet.searchForDataSource =
        [[SimpleComboBoxPersistentDataSource alloc] initWithStateConfigPath:g_ConfigSearchesPath];
    sheet.replaceWithDataSource =
        [[SimpleComboBoxPersistentDataSource alloc] initWithStateConfigPath:g_ConfigReplacesPath];

    __weak PanelController *weak_target = _target;
    __weak NCMainWindowController *weak_window_controller = window_controller;
    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      PanelController *const target = weak_target;
      NCMainWindowController *const current_window_controller = weak_window_controller;
      if( !target || !current_window_controller || target.mainWindowController != current_window_controller ||
          target.dataGeneration != data_generation || target.data.ListingPtr() != listing || target.vfs != host ||
          target.currentDirectoryPath != directory ||
          EvaluateBatchRenameSubmission(entries, target) != BatchRenameSubmissionResult::Presented ) {
          NSBeep();
          return;
      }

      std::vector<std::string> source_paths = sheet.filenamesSource;
      std::vector<std::string> destination_paths = sheet.filenamesDestination;
      try {
          const auto capabilities = vfs::ProviderCapabilitiesResolver::Resolve(*host, directory);
          const BatchRenameSubmissionResult plan = ValidatePlan(source_paths,
                                                                destination_paths,
                                                                entries,
                                                                target.data.Listing(),
                                                                directory,
                                                                capabilities.is_case_sensitive);
          if( plan != BatchRenameSubmissionResult::Presented ) {
              NSBeep();
              return;
          }

          const auto operation = std::make_shared<nc::ops::BatchRenaming>(
              std::move(source_paths), std::move(destination_paths), host);
          __weak PanelController *weak_refresh_target = target;
          operation->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [weak_refresh_target] {
              dispatch_to_main_queue([weak_refresh_target] {
                  if( PanelController *const panel = weak_refresh_target )
                      [panel hintAboutFilesystemChange];
              });
          });
          [current_window_controller enqueueOperation:operation];
      } catch( ... ) {
          NSBeep();
      }
    };

    [window_controller beginSheet:sheet.window completionHandler:handler];
    return BatchRenameSubmissionResult::Presented;
}

bool BatchRename::Predicate(PanelController *_target) const
{
    if( !_target )
        return false;
    const auto items = _target.selectedEntriesOrFocusedEntry;
    return EvaluateBatchRenameSubmission(items, _target) == BatchRenameSubmissionResult::Presented;
}

void BatchRename::Perform(PanelController *_target, id /*_sender*/) const
{
    if( !_target )
        return;
    const auto items = _target.selectedEntriesOrFocusedEntry;
    std::ignore = PresentBatchRename(items, _target);
}

} // namespace nc::panel::actions
