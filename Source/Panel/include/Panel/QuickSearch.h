// Copyright (C) 2018-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Config/Config.h>
#include <Panel/PanelViewKeystrokeSink.h>
#include <Panel/PanelDataFilter.h>

namespace nc::panel {
namespace data {
class Model;
}

namespace QuickSearch {

enum class KeyModif : int8_t { // persistancy-bound values, don't change it
    WithAlt = 0,
    WithCtrlAlt = 1,
    WithShiftAlt = 2,
    WithoutModif = 3,
    Disabled = 4
};

constexpr auto g_ConfigWhereToFind = "filePanel.quickSearch.whereToFind";
constexpr auto g_ConfigIsSoftFiltering = "filePanel.quickSearch.softFiltering";
constexpr auto g_ConfigTypingView = "filePanel.quickSearch.typingView";
constexpr auto g_ConfigKeyOption = "filePanel.quickSearch.keyOption";
constexpr auto g_ConfigIgnoreCharacters = "filePanel.quickSearch.ignoreCharacters";

} // namespace QuickSearch
} // namespace nc::panel

@class NCPanelQuickSearch;

@protocol NCPanelQuickSearchDelegate <NSObject>
@required

- (int)quickSearchNeedsCursorPosition:(NCPanelQuickSearch *)_qs;
- (void)quickSearch:(NCPanelQuickSearch *)_qs wantsToSetCursorPosition:(int)_cursor_position;
- (void)quickSearchHasChangedVolatileData:(NCPanelQuickSearch *)_qs;
- (void)quickSearchHasUpdatedData:(NCPanelQuickSearch *)_qs;
- (void)quickSearch:(NCPanelQuickSearch *)_qs wantsToSetSearchPrompt:(NSString *)_prompt withMatchesCount:(int)_count;

@optional

// Returns true when the delegate accepted a detached large-list preparation. Small-list delegates
// can omit these methods and keep the established synchronous Model behavior.
- (bool)quickSearch:(NCPanelQuickSearch *)_qs
    requestsDetachedHardFiltering:(const nc::panel::data::HardFilter &)_filter;
- (bool)quickSearch:(NCPanelQuickSearch *)_qs
    requestsDetachedSoftFiltering:(const nc::panel::data::TextualFilter &)_filter;
- (bool)quickSearchRequestsDetachedTextFilteringClear:(NCPanelQuickSearch *)_qs;

@end

@interface NCPanelQuickSearch : NSObject <NCPanelViewKeystrokeSink>

- (instancetype)initWithData:(nc::panel::data::Model &)_data
                    delegate:(NSObject<NCPanelQuickSearchDelegate> *)_delegate
                      config:(nc::config::Config &)_config;

- (void)setSearchCriteria:(NSString *)_request; // pass nil to discard filtering
- (NSString *)searchCriteria;                   // will return nil if there's no filtering

// Notifies the QuickSearch that the associated Model has reloaded its data.
// Potentially causes to update the search prompt UI
- (void)dataUpdated;

// Completion hooks for an accepted detached filter request. Stale requests never call commit.
- (void)detachedFilteringDidCommit;
- (void)detachedFilteringDidCancel;

@end
