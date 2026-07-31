// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PanelListViewProjection.h"
#include <Panel/PanelDataSortMode.h>
#include <VFS/VFS.h>
#include <Cocoa/Cocoa.h>
#include <ctime>
#include <optional>

namespace nc::panel {
namespace data {
class Model;
}

[[nodiscard]] std::vector<PanelListViewProjectionItem>
BuildPanelListViewProjectionItems(const data::Model &_data, time_t _now);

[[nodiscard]] NSString *PanelListViewGroupTitle(const PanelListViewGroupKey &_key);

[[nodiscard]] NSString *ExplorerFileTypeDescription(const VFSListingItem &_item);

} // namespace nc::panel
