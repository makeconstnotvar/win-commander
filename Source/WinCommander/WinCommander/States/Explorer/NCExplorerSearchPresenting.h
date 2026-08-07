// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, NCExplorerSearchPreferredScope) {
    NCExplorerSearchPreferredScopeCurrentFolder,
    NCExplorerSearchPreferredScopeSpotlightWholeMac,
};

/**
 * Narrow bridge used by the established Find Files actions to enter Explorer Search Mode.
 * Commander hosts do not implement this protocol and retain their existing sheet/popover flows.
 */
@protocol NCExplorerSearchPresenting <NSObject>

- (BOOL)canPresentSearchForPanel:(PanelController *)_panel;
- (BOOL)presentSearchForPanel:(PanelController *)_panel
                initialQuery:(nullable NSString *)_query
              preferredScope:(NCExplorerSearchPreferredScope)_scope;

@end

NS_ASSUME_NONNULL_END
