// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

namespace nc::ops {
class Pool;
}

@class NCExplorerOperationProgressView;

/** Copies thread-safe operation values from one exact window Pool into a value-only Explorer view. */
@interface ExplorerOperationProgressController : NSObject

- (instancetype)initWithPool:(nc::ops::Pool &)_pool view:(NCExplorerOperationProgressView *)_view;

@end
