// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerViewSettingsPersistence.h"

#include <Config/Config.h>
#include <Config/RapidJSON.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nc::explorer {

namespace {

using config::Value;
using namespace rapidjson;

constexpr auto g_SchemaKey = "schema";
constexpr auto g_EntriesKey = "entries";
constexpr auto g_FootprintKey = "footprint";
constexpr auto g_LocationKey = "location";
constexpr auto g_LocationPathKey = "path";
constexpr auto g_LocationHostsKey = "hosts_v1";
constexpr auto g_SettingsKey = "settings";
constexpr auto g_LayoutSlotKey = "layout_slot";
constexpr auto g_LayoutKey = "layout";
constexpr auto g_BriefKey = "brief";
constexpr auto g_ListKey = "list";
constexpr auto g_GalleryKey = "gallery";
constexpr auto g_ModeKey = "mode";
constexpr auto g_FixedWidthKey = "fixed_width";
constexpr auto g_FixedAmountKey = "fixed_amount";
constexpr auto g_DynamicMinKey = "dynamic_min";
constexpr auto g_DynamicMaxKey = "dynamic_max";
constexpr auto g_DynamicEqualKey = "dynamic_equal";
constexpr auto g_IconScaleKey = "icon_scale";
constexpr auto g_ColumnsKey = "columns";
constexpr auto g_KindKey = "kind";
constexpr auto g_WidthKey = "width";
constexpr auto g_MinWidthKey = "min_width";
constexpr auto g_MaxWidthKey = "max_width";
constexpr auto g_TextLinesKey = "text_lines";
constexpr auto g_SortKey = "sort";
constexpr auto g_DirectionKey = "direction";
constexpr auto g_CollationKey = "collation";
constexpr auto g_SeparatesDirectoriesKey = "separates_directories";
constexpr auto g_ExtensionlessDirectoriesKey = "extensionless_directories";
constexpr auto g_GroupingKey = "grouping";
constexpr auto g_EnabledKey = "enabled";

constexpr int g_LayoutSlots = 10;

struct Record {
    std::string footprint;
    Value location;
    ExplorerViewSettings settings;
};

using Records = std::vector<Record>;

Value String(const std::string_view _value)
{
    return config::MakeStandaloneString(_value);
}

const Value *Member(const Value &_object, const char *_key) noexcept
{
    if( !_object.IsObject() )
        return nullptr;
    const auto it = _object.FindMember(_key);
    return it == _object.MemberEnd() ? nullptr : &it->value;
}

std::optional<int> RequiredInt(const Value &_object, const char *_key) noexcept
{
    const auto value = Member(_object, _key);
    if( value == nullptr || !value->IsInt() )
        return std::nullopt;
    return value->GetInt();
}

std::optional<bool> RequiredBool(const Value &_object, const char *_key) noexcept
{
    const auto value = Member(_object, _key);
    if( value == nullptr || !value->IsBool() )
        return std::nullopt;
    return value->GetBool();
}

std::optional<std::string_view> RequiredString(const Value &_object, const char *_key) noexcept
{
    const auto value = Member(_object, _key);
    if( value == nullptr || !value->IsString() )
        return std::nullopt;
    return std::string_view{value->GetString(), value->GetStringLength()};
}

core::PaneGroupingKey GroupingKeyForSort(const core::PaneSortKey _key) noexcept
{
    using Group = core::PaneGroupingKey;
    using Sort = core::PaneSortKey;
    switch( _key ) {
        case Sort::Unsorted:
        case Sort::RawName:
        case Sort::Name:
            return Group::Name;
        case Sort::Extension:
            return Group::Extension;
        case Sort::Size:
            return Group::Size;
        case Sort::ModifiedTime:
            return Group::ModifiedTime;
        case Sort::CreatedTime:
            return Group::CreatedTime;
        case Sort::AddedTime:
            return Group::AddedTime;
        case Sort::AccessedTime:
            return Group::AccessedTime;
        case Sort::Unknown:
        default:
            return Group::Unknown;
    }
}

bool IsValidSort(const core::PaneSortState &_sort) noexcept
{
    using Collation = core::PaneTextCollation;
    using Direction = core::PaneSortDirection;
    using Sort = core::PaneSortKey;

    if( _sort.collation != Collation::Natural && _sort.collation != Collation::CaseInsensitive &&
        _sort.collation != Collation::CaseSensitive )
        return false;
    switch( _sort.key ) {
        case Sort::Unsorted:
            return _sort.direction == Direction::None;
        case Sort::RawName:
            return _sort.direction == Direction::Ascending;
        case Sort::Name:
        case Sort::Extension:
        case Sort::Size:
        case Sort::ModifiedTime:
        case Sort::CreatedTime:
        case Sort::AddedTime:
        case Sort::AccessedTime:
            return _sort.direction == Direction::Ascending || _sort.direction == Direction::Descending;
        case Sort::Unknown:
        default:
            return false;
    }
}

bool IsValidGrouping(const core::PaneSortState &_sort, const core::PaneGroupingState &_grouping) noexcept
{
    if( !_grouping.enabled )
        return _grouping.key == core::PaneGroupingKey::Unknown;
    const auto expected = GroupingKeyForSort(_sort.key);
    return expected != core::PaneGroupingKey::Unknown && _grouping.key == expected;
}

bool IsValidSettings(const ExplorerViewSettings &_settings) noexcept
{
    if( _settings.layout_slot < 0 || _settings.layout_slot >= g_LayoutSlots || !IsValidSort(_settings.sort) ||
        !IsValidGrouping(_settings.sort, _settings.grouping) )
        return false;
    return panel::IsValidPanelViewLayout(_settings.layout);
}

std::optional<std::string_view> BriefModeName(const panel::PanelBriefViewColumnsLayout::Mode _mode) noexcept
{
    using Mode = panel::PanelBriefViewColumnsLayout::Mode;
    switch( _mode ) {
        case Mode::FixedWidth:
            return "fixed_width";
        case Mode::FixedAmount:
            return "fixed_amount";
        case Mode::DynamicWidth:
            return "dynamic_width";
        default:
            return std::nullopt;
    }
}

std::optional<panel::PanelBriefViewColumnsLayout::Mode> ParseBriefMode(const std::string_view _mode) noexcept
{
    using Mode = panel::PanelBriefViewColumnsLayout::Mode;
    if( _mode == "fixed_width" )
        return Mode::FixedWidth;
    if( _mode == "fixed_amount" )
        return Mode::FixedAmount;
    if( _mode == "dynamic_width" )
        return Mode::DynamicWidth;
    return std::nullopt;
}

std::optional<std::string_view> SortKeyName(const core::PaneSortKey _key) noexcept
{
    using Sort = core::PaneSortKey;
    switch( _key ) {
        case Sort::Unsorted:
            return "unsorted";
        case Sort::RawName:
            return "raw_name";
        case Sort::Name:
            return "name";
        case Sort::Extension:
            return "extension";
        case Sort::Size:
            return "size";
        case Sort::ModifiedTime:
            return "modified_time";
        case Sort::CreatedTime:
            return "created_time";
        case Sort::AddedTime:
            return "added_time";
        case Sort::AccessedTime:
            return "accessed_time";
        case Sort::Unknown:
        default:
            return std::nullopt;
    }
}

std::optional<core::PaneSortKey> ParseSortKey(const std::string_view _key) noexcept
{
    using Sort = core::PaneSortKey;
    if( _key == "unsorted" )
        return Sort::Unsorted;
    if( _key == "raw_name" )
        return Sort::RawName;
    if( _key == "name" )
        return Sort::Name;
    if( _key == "extension" )
        return Sort::Extension;
    if( _key == "size" )
        return Sort::Size;
    if( _key == "modified_time" )
        return Sort::ModifiedTime;
    if( _key == "created_time" )
        return Sort::CreatedTime;
    if( _key == "added_time" )
        return Sort::AddedTime;
    if( _key == "accessed_time" )
        return Sort::AccessedTime;
    return std::nullopt;
}

std::optional<std::string_view> DirectionName(const core::PaneSortDirection _direction) noexcept
{
    using Direction = core::PaneSortDirection;
    switch( _direction ) {
        case Direction::None:
            return "none";
        case Direction::Ascending:
            return "ascending";
        case Direction::Descending:
            return "descending";
        default:
            return std::nullopt;
    }
}

std::optional<core::PaneSortDirection> ParseDirection(const std::string_view _direction) noexcept
{
    using Direction = core::PaneSortDirection;
    if( _direction == "none" )
        return Direction::None;
    if( _direction == "ascending" )
        return Direction::Ascending;
    if( _direction == "descending" )
        return Direction::Descending;
    return std::nullopt;
}

std::optional<std::string_view> CollationName(const core::PaneTextCollation _collation) noexcept
{
    using Collation = core::PaneTextCollation;
    switch( _collation ) {
        case Collation::Natural:
            return "natural";
        case Collation::CaseInsensitive:
            return "case_insensitive";
        case Collation::CaseSensitive:
            return "case_sensitive";
        case Collation::Unknown:
        default:
            return std::nullopt;
    }
}

std::optional<core::PaneTextCollation> ParseCollation(const std::string_view _collation) noexcept
{
    using Collation = core::PaneTextCollation;
    if( _collation == "natural" )
        return Collation::Natural;
    if( _collation == "case_insensitive" )
        return Collation::CaseInsensitive;
    if( _collation == "case_sensitive" )
        return Collation::CaseSensitive;
    return std::nullopt;
}

std::optional<std::string_view> GroupingKeyName(const core::PaneGroupingKey _key) noexcept
{
    using Group = core::PaneGroupingKey;
    switch( _key ) {
        case Group::Unknown:
            return "none";
        case Group::Name:
            return "name";
        case Group::Extension:
            return "extension";
        case Group::Size:
            return "size";
        case Group::ModifiedTime:
            return "modified_time";
        case Group::CreatedTime:
            return "created_time";
        case Group::AddedTime:
            return "added_time";
        case Group::AccessedTime:
            return "accessed_time";
        default:
            return std::nullopt;
    }
}

std::optional<core::PaneGroupingKey> ParseGroupingKey(const std::string_view _key) noexcept
{
    using Group = core::PaneGroupingKey;
    if( _key == "none" )
        return Group::Unknown;
    if( _key == "name" )
        return Group::Name;
    if( _key == "extension" )
        return Group::Extension;
    if( _key == "size" )
        return Group::Size;
    if( _key == "modified_time" )
        return Group::ModifiedTime;
    if( _key == "created_time" )
        return Group::CreatedTime;
    if( _key == "added_time" )
        return Group::AddedTime;
    if( _key == "accessed_time" )
        return Group::AccessedTime;
    return std::nullopt;
}

Value EncodeLayout(const panel::PanelViewLayout &_layout)
{
    Value result{kObjectType};
    if( const auto brief = _layout.brief() ) {
        Value value{kObjectType};
        value.AddMember(String(g_ModeKey), String(*BriefModeName(brief->mode)), config::g_CrtAllocator);
        value.AddMember(String(g_FixedWidthKey), Value{brief->fixed_mode_width}, config::g_CrtAllocator);
        value.AddMember(String(g_FixedAmountKey), Value{brief->fixed_amount_value}, config::g_CrtAllocator);
        value.AddMember(String(g_DynamicMinKey), Value{brief->dynamic_width_min}, config::g_CrtAllocator);
        value.AddMember(String(g_DynamicMaxKey), Value{brief->dynamic_width_max}, config::g_CrtAllocator);
        value.AddMember(String(g_DynamicEqualKey), Value{brief->dynamic_width_equal}, config::g_CrtAllocator);
        value.AddMember(String(g_IconScaleKey), Value{brief->icon_scale}, config::g_CrtAllocator);
        result.AddMember(String(g_BriefKey), std::move(value), config::g_CrtAllocator);
    }
    else if( const auto list = _layout.list() ) {
        Value value{kObjectType};
        Value columns{kArrayType};
        for( const auto &column : list->columns ) {
            Value encoded{kObjectType};
            encoded.AddMember(String(g_KindKey), Value{static_cast<int>(column.kind)}, config::g_CrtAllocator);
            encoded.AddMember(String(g_WidthKey), Value{column.width}, config::g_CrtAllocator);
            encoded.AddMember(String(g_MinWidthKey), Value{column.min_width}, config::g_CrtAllocator);
            encoded.AddMember(String(g_MaxWidthKey), Value{column.max_width}, config::g_CrtAllocator);
            columns.PushBack(std::move(encoded), config::g_CrtAllocator);
        }
        value.AddMember(String(g_ColumnsKey), std::move(columns), config::g_CrtAllocator);
        value.AddMember(String(g_IconScaleKey), Value{list->icon_scale}, config::g_CrtAllocator);
        result.AddMember(String(g_ListKey), std::move(value), config::g_CrtAllocator);
    }
    else if( const auto gallery = _layout.gallery() ) {
        Value value{kObjectType};
        value.AddMember(String(g_IconScaleKey), Value{gallery->icon_scale}, config::g_CrtAllocator);
        value.AddMember(String(g_TextLinesKey), Value{gallery->text_lines}, config::g_CrtAllocator);
        result.AddMember(String(g_GalleryKey), std::move(value), config::g_CrtAllocator);
    }
    return result;
}

std::optional<panel::PanelViewLayout> DecodeLayout(const Value &_value)
{
    if( !_value.IsObject() || _value.MemberCount() != 1 )
        return std::nullopt;

    panel::PanelViewLayout result;
    if( const auto brief_value = Member(_value, g_BriefKey); brief_value != nullptr && brief_value->IsObject() ) {
        if( brief_value->MemberCount() != 7 )
            return std::nullopt;
        const auto mode_name = RequiredString(*brief_value, g_ModeKey);
        const auto fixed_width = RequiredInt(*brief_value, g_FixedWidthKey);
        const auto fixed_amount = RequiredInt(*brief_value, g_FixedAmountKey);
        const auto dynamic_min = RequiredInt(*brief_value, g_DynamicMinKey);
        const auto dynamic_max = RequiredInt(*brief_value, g_DynamicMaxKey);
        const auto dynamic_equal = RequiredBool(*brief_value, g_DynamicEqualKey);
        const auto icon_scale = RequiredInt(*brief_value, g_IconScaleKey);
        if( !mode_name || !fixed_width || !fixed_amount || !dynamic_min || !dynamic_max || !dynamic_equal ||
            !icon_scale || *fixed_width < std::numeric_limits<short>::min() ||
            *fixed_width > std::numeric_limits<short>::max() || *fixed_amount < std::numeric_limits<short>::min() ||
            *fixed_amount > std::numeric_limits<short>::max() || *dynamic_min < std::numeric_limits<short>::min() ||
            *dynamic_min > std::numeric_limits<short>::max() || *dynamic_max < std::numeric_limits<short>::min() ||
            *dynamic_max > std::numeric_limits<short>::max() || *icon_scale < 0 || *icon_scale > 3 )
            return std::nullopt;
        const auto mode = ParseBriefMode(*mode_name);
        if( !mode )
            return std::nullopt;
        panel::PanelBriefViewColumnsLayout brief;
        brief.mode = *mode;
        brief.fixed_mode_width = static_cast<short>(*fixed_width);
        brief.fixed_amount_value = static_cast<short>(*fixed_amount);
        brief.dynamic_width_min = static_cast<short>(*dynamic_min);
        brief.dynamic_width_max = static_cast<short>(*dynamic_max);
        brief.dynamic_width_equal = *dynamic_equal;
        brief.icon_scale = static_cast<unsigned char>(*icon_scale);
        result.layout = brief;
    }
    else if( const auto list_value = Member(_value, g_ListKey); list_value != nullptr && list_value->IsObject() ) {
        if( list_value->MemberCount() != 2 )
            return std::nullopt;
        const auto columns = Member(*list_value, g_ColumnsKey);
        const auto icon_scale = RequiredInt(*list_value, g_IconScaleKey);
        if( columns == nullptr || !columns->IsArray() || columns->Size() > 8 || !icon_scale || *icon_scale < 0 ||
            *icon_scale > 255 )
            return std::nullopt;
        panel::PanelListViewColumnsLayout list;
        for( const auto &encoded : columns->GetArray() ) {
            if( !encoded.IsObject() || encoded.MemberCount() != 4 )
                return std::nullopt;
            const auto kind = RequiredInt(encoded, g_KindKey);
            const auto width = RequiredInt(encoded, g_WidthKey);
            const auto min_width = RequiredInt(encoded, g_MinWidthKey);
            const auto max_width = RequiredInt(encoded, g_MaxWidthKey);
            if( !kind || !width || !min_width || !max_width || *kind < std::numeric_limits<signed char>::min() ||
                *kind > std::numeric_limits<signed char>::max() || *width < std::numeric_limits<short>::min() ||
                *width > std::numeric_limits<short>::max() || *min_width < std::numeric_limits<short>::min() ||
                *min_width > std::numeric_limits<short>::max() || *max_width < std::numeric_limits<short>::min() ||
                *max_width > std::numeric_limits<short>::max() )
                return std::nullopt;
            list.columns.emplace_back(
                panel::PanelListViewColumnsLayout::Column{.kind = static_cast<panel::PanelListViewColumns>(*kind),
                                                          .width = static_cast<short>(*width),
                                                          .max_width = static_cast<short>(*max_width),
                                                          .min_width = static_cast<short>(*min_width)});
        }
        list.icon_scale = static_cast<unsigned char>(*icon_scale);
        result.layout = list;
    }
    else if( const auto gallery_value = Member(_value, g_GalleryKey);
             gallery_value != nullptr && gallery_value->IsObject() ) {
        if( gallery_value->MemberCount() != 2 )
            return std::nullopt;
        const auto icon_scale = RequiredInt(*gallery_value, g_IconScaleKey);
        const auto text_lines = RequiredInt(*gallery_value, g_TextLinesKey);
        if( !icon_scale || !text_lines || *icon_scale < 0 || *icon_scale > 255 || *text_lines < 0 || *text_lines > 255 )
            return std::nullopt;
        result.layout = panel::PanelGalleryViewLayout{.icon_scale = static_cast<unsigned char>(*icon_scale),
                                                      .text_lines = static_cast<unsigned char>(*text_lines)};
    }
    else {
        return std::nullopt;
    }
    return IsValidSettings(ExplorerViewSettings{.layout_slot = 0,
                                                .layout = result,
                                                .sort = {.key = core::PaneSortKey::Name,
                                                         .direction = core::PaneSortDirection::Ascending,
                                                         .collation = core::PaneTextCollation::Natural},
                                                .grouping = {}})
               ? std::optional{std::move(result)}
               : std::nullopt;
}

Value EncodeSort(const core::PaneSortState &_sort)
{
    Value result{kObjectType};
    result.AddMember(String(g_SortKey), String(*SortKeyName(_sort.key)), config::g_CrtAllocator);
    result.AddMember(String(g_DirectionKey), String(*DirectionName(_sort.direction)), config::g_CrtAllocator);
    result.AddMember(String(g_CollationKey), String(*CollationName(_sort.collation)), config::g_CrtAllocator);
    result.AddMember(String(g_SeparatesDirectoriesKey), Value{_sort.separates_directories}, config::g_CrtAllocator);
    result.AddMember(
        String(g_ExtensionlessDirectoriesKey), Value{_sort.extensionless_directories}, config::g_CrtAllocator);
    return result;
}

std::optional<core::PaneSortState> DecodeSort(const Value &_value)
{
    if( !_value.IsObject() || _value.MemberCount() != 5 )
        return std::nullopt;
    const auto key_name = RequiredString(_value, g_SortKey);
    const auto direction_name = RequiredString(_value, g_DirectionKey);
    const auto collation_name = RequiredString(_value, g_CollationKey);
    const auto separates_directories = RequiredBool(_value, g_SeparatesDirectoriesKey);
    const auto extensionless_directories = RequiredBool(_value, g_ExtensionlessDirectoriesKey);
    if( !key_name || !direction_name || !collation_name || !separates_directories || !extensionless_directories )
        return std::nullopt;
    const auto key = ParseSortKey(*key_name);
    const auto direction = ParseDirection(*direction_name);
    const auto collation = ParseCollation(*collation_name);
    if( !key || !direction || !collation )
        return std::nullopt;
    core::PaneSortState result{.key = *key,
                               .direction = *direction,
                               .collation = *collation,
                               .separates_directories = *separates_directories,
                               .extensionless_directories = *extensionless_directories};
    return IsValidSort(result) ? std::optional{result} : std::nullopt;
}

Value EncodeGrouping(const core::PaneGroupingState &_grouping)
{
    Value result{kObjectType};
    result.AddMember(String(g_EnabledKey), Value{_grouping.enabled}, config::g_CrtAllocator);
    result.AddMember(String(g_KindKey), String(*GroupingKeyName(_grouping.key)), config::g_CrtAllocator);
    return result;
}

std::optional<core::PaneGroupingState> DecodeGrouping(const Value &_value)
{
    if( !_value.IsObject() || _value.MemberCount() != 2 )
        return std::nullopt;
    const auto enabled = RequiredBool(_value, g_EnabledKey);
    const auto key_name = RequiredString(_value, g_KindKey);
    if( !enabled || !key_name )
        return std::nullopt;
    const auto key = ParseGroupingKey(*key_name);
    if( !key )
        return std::nullopt;
    return core::PaneGroupingState{.enabled = *enabled, .key = *key};
}

Value EncodeSettings(const ExplorerViewSettings &_settings)
{
    Value result{kObjectType};
    result.AddMember(String(g_LayoutSlotKey), Value{_settings.layout_slot}, config::g_CrtAllocator);
    result.AddMember(String(g_LayoutKey), EncodeLayout(_settings.layout), config::g_CrtAllocator);
    result.AddMember(String(g_SortKey), EncodeSort(_settings.sort), config::g_CrtAllocator);
    result.AddMember(String(g_GroupingKey), EncodeGrouping(_settings.grouping), config::g_CrtAllocator);
    return result;
}

std::optional<ExplorerViewSettings> DecodeSettings(const Value &_value)
{
    if( !_value.IsObject() || _value.MemberCount() != 4 )
        return std::nullopt;
    const auto slot = RequiredInt(_value, g_LayoutSlotKey);
    const auto layout_value = Member(_value, g_LayoutKey);
    const auto sort_value = Member(_value, g_SortKey);
    const auto grouping_value = Member(_value, g_GroupingKey);
    if( !slot || layout_value == nullptr || sort_value == nullptr || grouping_value == nullptr )
        return std::nullopt;
    auto layout = DecodeLayout(*layout_value);
    auto sort = DecodeSort(*sort_value);
    auto grouping = DecodeGrouping(*grouping_value);
    if( !layout || !sort || !grouping )
        return std::nullopt;
    ExplorerViewSettings result{
        .layout_slot = *slot, .layout = std::move(*layout), .sort = *sort, .grouping = *grouping};
    return IsValidSettings(result) ? std::optional{std::move(result)} : std::nullopt;
}

Value EncodeRecord(const Record &_record)
{
    Value result{kObjectType};
    result.AddMember(String(g_FootprintKey), String(_record.footprint), config::g_CrtAllocator);
    result.AddMember(String(g_LocationKey), Value{_record.location, config::g_CrtAllocator}, config::g_CrtAllocator);
    result.AddMember(String(g_SettingsKey), EncodeSettings(_record.settings), config::g_CrtAllocator);
    return result;
}

std::optional<Record> DecodeRecord(const Value &_value, panel::PanelDataPersistency &_persistency)
{
    if( !_value.IsObject() || _value.MemberCount() != 3 )
        return std::nullopt;
    const auto footprint = RequiredString(_value, g_FootprintKey);
    const auto location = Member(_value, g_LocationKey);
    const auto settings = Member(_value, g_SettingsKey);
    if( !footprint || footprint->empty() || location == nullptr || settings == nullptr )
        return std::nullopt;

    if( !location->IsObject() )
        return std::nullopt;
    const auto path_member = Member(*location, g_LocationPathKey);
    if( path_member == nullptr || !path_member->IsString() )
        return std::nullopt;
    if( const auto hosts = Member(*location, g_LocationHostsKey) ) {
        if( !hosts->IsArray() )
            return std::nullopt;
        for( const auto &host : hosts->GetArray() )
            if( !host.IsObject() )
                return std::nullopt;
    }

    const auto decoded_location = _persistency.JSONToLocation(*location);
    if( !decoded_location || decoded_location->path.empty() || decoded_location->path.back() != '/' )
        return std::nullopt;
    const auto canonical_location = _persistency.LocationToJSON(*decoded_location);
    if( !canonical_location.IsObject() || canonical_location != *location )
        return std::nullopt;

    auto decoded_settings = DecodeSettings(*settings);
    if( !decoded_settings )
        return std::nullopt;
    return Record{.footprint = std::string{*footprint},
                  .location = Value{*location, config::g_CrtAllocator},
                  .settings = std::move(*decoded_settings)};
}

std::optional<Records> DecodeRoot(const Value &_root, panel::PanelDataPersistency &_persistency)
{
    if( !_root.IsObject() || _root.MemberCount() != 2 )
        return std::nullopt;
    const auto schema = RequiredInt(_root, g_SchemaKey);
    const auto entries = Member(_root, g_EntriesKey);
    if( !schema || *schema != ExplorerViewSettingsPersistence::SchemaVersion || entries == nullptr ||
        !entries->IsArray() || entries->Size() > ExplorerViewSettingsPersistence::Capacity )
        return std::nullopt;

    Records result;
    result.reserve(entries->Size());
    for( const auto &entry : entries->GetArray() ) {
        auto decoded = DecodeRecord(entry, _persistency);
        if( !decoded )
            return std::nullopt;
        if( std::ranges::any_of(result,
                                [&](const Record &_existing) { return _existing.location == decoded->location; }) )
            return std::nullopt;
        result.emplace_back(std::move(*decoded));
    }
    return result;
}

Value EncodeRoot(const Records &_records)
{
    Value root{kObjectType};
    root.AddMember(String(g_SchemaKey), Value{ExplorerViewSettingsPersistence::SchemaVersion}, config::g_CrtAllocator);
    Value entries{kArrayType};
    entries.Reserve(static_cast<SizeType>(_records.size()), config::g_CrtAllocator);
    for( const auto &record : _records )
        entries.PushBack(EncodeRecord(record), config::g_CrtAllocator);
    root.AddMember(String(g_EntriesKey), std::move(entries), config::g_CrtAllocator);
    return root;
}

std::optional<Value> CanonicalLocation(const panel::PersistentLocation &_location,
                                       panel::PanelDataPersistency &_persistency)
{
    if( _location.path.empty() || _location.path.back() != '/' )
        return std::nullopt;
    auto encoded = _persistency.LocationToJSON(_location);
    if( !encoded.IsObject() )
        return std::nullopt;
    const auto decoded = _persistency.JSONToLocation(encoded);
    if( !decoded || _persistency.LocationToJSON(*decoded) != encoded )
        return std::nullopt;
    return encoded;
}

std::optional<size_t>
FindExactRecord(const Records &_records, const std::string_view _footprint, const Value &_location) noexcept
{
    for( size_t index = 0; index < _records.size(); ++index )
        if( _records[index].footprint == _footprint && _records[index].location == _location )
            return index;
    for( size_t index = 0; index < _records.size(); ++index )
        if( _records[index].location == _location )
            return index;
    return std::nullopt;
}

} // namespace

ExplorerViewSettingsPersistence::ExplorerViewSettingsPersistence(config::Config &_config,
                                                                 panel::PanelDataPersistency &_location_persistency)
    : m_Config(_config), m_LocationPersistency(_location_persistency)
{
}

std::optional<ExplorerViewSettings> ExplorerViewSettingsPersistence::Load(const panel::PersistentLocation &_location)
{
    const auto lock = std::lock_guard{m_Mutex};
    const auto location = CanonicalLocation(_location, m_LocationPersistency);
    if( !location || !m_Config.Has(ConfigPath) )
        return std::nullopt;

    auto records = DecodeRoot(m_Config.Get(ConfigPath), m_LocationPersistency);
    if( !records )
        return std::nullopt;

    const auto footprint = m_LocationPersistency.MakeFootprintString(_location);
    const auto index = FindExactRecord(*records, footprint, *location);
    if( !index )
        return std::nullopt;

    ExplorerViewSettings result = (*records)[*index].settings;
    if( *index != 0 || (*records)[*index].footprint != footprint ) {
        Record record = std::move((*records)[*index]);
        record.footprint = footprint;
        records->erase(records->begin() + static_cast<std::ptrdiff_t>(*index));
        records->insert(records->begin(), std::move(record));
        m_Config.Set(ConfigPath, EncodeRoot(*records));
    }
    return result;
}

bool ExplorerViewSettingsPersistence::Store(const panel::PersistentLocation &_location,
                                            const ExplorerViewSettings &_settings)
{
    const auto lock = std::lock_guard{m_Mutex};
    if( !IsValidSettings(_settings) )
        return false;
    auto location = CanonicalLocation(_location, m_LocationPersistency);
    if( !location )
        return false;

    Records records;
    if( m_Config.Has(ConfigPath) ) {
        auto decoded = DecodeRoot(m_Config.Get(ConfigPath), m_LocationPersistency);
        if( !decoded )
            return false;
        records = std::move(*decoded);
    }

    const auto footprint = m_LocationPersistency.MakeFootprintString(_location);
    if( const auto index = FindExactRecord(records, footprint, *location) )
        records.erase(records.begin() + static_cast<std::ptrdiff_t>(*index));
    records.insert(records.begin(),
                   Record{.footprint = footprint, .location = std::move(*location), .settings = _settings});
    if( records.size() > Capacity )
        records.resize(Capacity);

    const auto root = EncodeRoot(records);
    m_Config.Set(ConfigPath, root);
    return m_Config.Has(ConfigPath) && m_Config.Get(ConfigPath) == root;
}

} // namespace nc::explorer
