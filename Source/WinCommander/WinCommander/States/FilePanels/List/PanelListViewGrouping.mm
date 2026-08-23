// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelListViewGrouping.h"
#include <Panel/PanelData.h>
#include <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <algorithm>
#include <cctype>

namespace nc::panel {

static NSString *ExplorerTypeDescriptionForExtension(NSString *_extension)
{
    if( _extension.length == 0 )
        return NSLocalizedString(@"File", "Explorer Details type for an extensionless file");

    [[clang::no_destroy]] static NSMutableDictionary<NSString *, NSString *> *const cache =
        [NSMutableDictionary new];
    NSString *const key = _extension.lowercaseString;
    if( NSString *const cached = cache[key] )
        return cached;

    NSString *description = nil;
    if( @available(macOS 11.0, *) ) {
        if( UTType *const type = [UTType typeWithFilenameExtension:key] )
            if( !type.dynamic && type.localizedDescription.length )
                description = type.localizedDescription;
    }
    if( description.length == 0 )
        description = [NSString stringWithFormat:NSLocalizedString(@"%@ file", "Explorer type for unknown extension"),
                                                   _extension.uppercaseString];
    cache[key] = description;
    return description;
}

NSString *ExplorerFileTypeDescription(const VFSListingItem &_item)
{
    // An empty item has no listing behind it; every accessor below would read through a null one.
    if( !_item )
        return @"";

    if( _item.IsDir() )
        return NSLocalizedString(@"File folder", "Explorer Details type for a directory");
    NSString *const extension = _item.HasExtension() ? [NSString stringWithUTF8String:_item.Extension()] : nil;
    return ExplorerTypeDescriptionForExtension(extension);
}

static std::string ToUTF8(NSString *_string)
{
    return _string.UTF8String ? _string.UTF8String : "";
}

static PanelListViewGroupKey NameGroup(const VFSListingItem &_item)
{
    NSString *const name = _item.DisplayNameNS();
    if( name.length == 0 )
        return {PanelListViewGroupKind::Unknown, {}};

    const NSRange composed = [name rangeOfComposedCharacterSequenceAtIndex:0];
    NSString *const first = [name substringWithRange:composed];
    if( [first rangeOfCharacterFromSet:NSCharacterSet.letterCharacterSet].location == NSNotFound )
        return {PanelListViewGroupKind::NameInitial, "#"};
    return {PanelListViewGroupKind::NameInitial, ToUTF8(first.localizedUppercaseString)};
}

static PanelListViewGroupKey TypeGroup(const VFSListingItem &_item)
{
    if( _item.IsDir() )
        return {PanelListViewGroupKind::Folders, {}};
    return {PanelListViewGroupKind::Type, ToUTF8(ExplorerFileTypeDescription(_item))};
}

static PanelListViewGroupKey SizeGroup(const VFSListingItem &_item)
{
    if( _item.IsDir() )
        return {PanelListViewGroupKind::Folders, {}};
    if( !_item.HasSize() )
        return {PanelListViewGroupKind::Unknown, {}};

    const auto size = _item.Size();
    if( size == 0 )
        return {PanelListViewGroupKind::Empty, {}};
    if( size < 16ULL * 1024ULL )
        return {PanelListViewGroupKind::Tiny, {}};
    if( size < 1024ULL * 1024ULL )
        return {PanelListViewGroupKind::Small, {}};
    if( size < 128ULL * 1024ULL * 1024ULL )
        return {PanelListViewGroupKind::Medium, {}};
    if( size < 1024ULL * 1024ULL * 1024ULL )
        return {PanelListViewGroupKind::Large, {}};
    return {PanelListViewGroupKind::Huge, {}};
}

static std::optional<time_t> ItemTimeForSortMode(const VFSListingItem &_item, data::SortMode::Mode _mode)
{
    using Mode = data::SortMode::Mode;
    switch( _mode ) {
        case Mode::SortByModTime:
        case Mode::SortByModTimeRev:
            return _item.HasMTime() ? std::optional{_item.MTime()} : std::nullopt;
        case Mode::SortByBirthTime:
        case Mode::SortByBirthTimeRev:
            return _item.HasBTime() ? std::optional{_item.BTime()} : std::nullopt;
        case Mode::SortByAddTime:
        case Mode::SortByAddTimeRev:
            return _item.HasAddTime() ? std::optional{_item.AddTime()} : std::nullopt;
        case Mode::SortByAccessTime:
        case Mode::SortByAccessTimeRev:
            return _item.HasATime() ? std::optional{_item.ATime()} : std::nullopt;
        default:
            return std::nullopt;
    }
}

static PanelListViewGroupKey DateGroup(const VFSListingItem &_item, data::SortMode::Mode _mode, time_t _now)
{
    const auto item_time = ItemTimeForSortMode(_item, _mode);
    if( !item_time )
        return {PanelListViewGroupKind::Unknown, {}};

    std::tm now_tm{};
    std::tm item_tm{};
    localtime_r(&_now, &now_tm);
    const time_t value = *item_time;
    localtime_r(&value, &item_tm);

    if( now_tm.tm_year == item_tm.tm_year && now_tm.tm_yday == item_tm.tm_yday )
        return {PanelListViewGroupKind::Today, {}};

    std::tm now_noon = now_tm;
    std::tm item_noon = item_tm;
    now_noon.tm_hour = item_noon.tm_hour = 12;
    now_noon.tm_min = item_noon.tm_min = 0;
    now_noon.tm_sec = item_noon.tm_sec = 0;
    const int days = static_cast<int>(std::difftime(std::mktime(&now_noon), std::mktime(&item_noon)) / 86400.0);

    if( days <= 0 )
        return {PanelListViewGroupKind::Today, {}};
    if( days == 1 )
        return {PanelListViewGroupKind::Yesterday, {}};
    if( days <= 7 )
        return {PanelListViewGroupKind::EarlierThisWeek, {}};
    if( days <= 31 )
        return {PanelListViewGroupKind::EarlierThisMonth, {}};
    if( now_tm.tm_year == item_tm.tm_year )
        return {PanelListViewGroupKind::EarlierThisYear, {}};
    return {PanelListViewGroupKind::LongAgo, {}};
}

static PanelListViewGroupKey GroupForItem(const VFSListingItem &_item, data::SortMode _sort, time_t _now)
{
    if( _sort.sep_dirs && _item.IsDir() )
        return {PanelListViewGroupKind::Folders, {}};

    using Mode = data::SortMode::Mode;
    switch( _sort.sort ) {
        case Mode::SortByExt:
        case Mode::SortByExtRev:
            return TypeGroup(_item);
        case Mode::SortBySize:
        case Mode::SortBySizeRev:
            return SizeGroup(_item);
        case Mode::SortByModTime:
        case Mode::SortByModTimeRev:
        case Mode::SortByBirthTime:
        case Mode::SortByBirthTimeRev:
        case Mode::SortByAddTime:
        case Mode::SortByAddTimeRev:
        case Mode::SortByAccessTime:
        case Mode::SortByAccessTimeRev:
            return DateGroup(_item, _sort.sort, _now);
        default:
            return NameGroup(_item);
    }
}

std::vector<PanelListViewProjectionItem> BuildPanelListViewProjectionItems(const data::Model &_data, time_t _now)
{
    const int count = _data.SortedEntriesCount();
    std::vector<PanelListViewProjectionItem> items;
    items.reserve(count);
    const auto sort = _data.SortMode();
    for( int sorted_index = 0; sorted_index < count; ++sorted_index ) {
        const auto item = _data.EntryAtSortPosition(sorted_index);
        if( !item )
            continue;
        items.emplace_back(PanelListViewProjectionItem{
            .sorted_index = sorted_index,
            .group = GroupForItem(item, sort, _now),
            .is_dotdot = item.IsDotDot(),
        });
    }
    return items;
}

NSString *PanelListViewGroupTitle(const PanelListViewGroupKey &_key)
{
    using Kind = PanelListViewGroupKind;
    switch( _key.kind ) {
        case Kind::Folders:
            return NSLocalizedString(@"Folders", "Explorer Details group title");
        case Kind::NameInitial:
        case Kind::Type:
            return [NSString stringWithUTF8String:_key.value.c_str()];
        case Kind::Empty:
            return NSLocalizedString(@"Empty", "Explorer Details size group");
        case Kind::Tiny:
            return NSLocalizedString(@"Tiny (under 16 KB)", "Explorer Details size group");
        case Kind::Small:
            return NSLocalizedString(@"Small (16 KB – 1 MB)", "Explorer Details size group");
        case Kind::Medium:
            return NSLocalizedString(@"Medium (1 MB – 128 MB)", "Explorer Details size group");
        case Kind::Large:
            return NSLocalizedString(@"Large (128 MB – 1 GB)", "Explorer Details size group");
        case Kind::Huge:
            return NSLocalizedString(@"Huge (1 GB and larger)", "Explorer Details size group");
        case Kind::Today:
            return NSLocalizedString(@"Today", "Explorer Details date group");
        case Kind::Yesterday:
            return NSLocalizedString(@"Yesterday", "Explorer Details date group");
        case Kind::EarlierThisWeek:
            return NSLocalizedString(@"Earlier this week", "Explorer Details date group");
        case Kind::EarlierThisMonth:
            return NSLocalizedString(@"Earlier this month", "Explorer Details date group");
        case Kind::EarlierThisYear:
            return NSLocalizedString(@"Earlier this year", "Explorer Details date group");
        case Kind::LongAgo:
            return NSLocalizedString(@"Long ago", "Explorer Details date group");
        case Kind::Unknown:
            return NSLocalizedString(@"Other", "Explorer Details group title");
    }
}

} // namespace nc::panel
