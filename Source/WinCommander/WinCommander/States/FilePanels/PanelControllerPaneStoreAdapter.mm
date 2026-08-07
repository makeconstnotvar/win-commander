// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelControllerPaneStoreAdapter.h"

#include "PanelController.h"
#include "PanelHistory.h"
#include "PanelView.h"
#include <Base/algo.h>
#include <Base/dispatch_cpp.h>
#include <Panel/PanelData.h>
#include <Panel/Log.h>
#include <limits>
#include <stdexcept>

namespace nc::panel {

core::PaneSortState ProjectPaneSortState(const data::SortMode _sort_mode) noexcept
{
    using LegacyMode = data::SortMode::Mode;
    using Key = core::PaneSortKey;
    using Direction = core::PaneSortDirection;

    core::PaneSortState state;
    state.separates_directories = _sort_mode.sep_dirs;
    state.extensionless_directories = _sort_mode.extensionless_dirs;
    switch( _sort_mode.collation ) {
        case data::SortMode::Collation::Natural:
            state.collation = core::PaneTextCollation::Natural;
            break;
        case data::SortMode::Collation::CaseInsensitive:
            state.collation = core::PaneTextCollation::CaseInsensitive;
            break;
        case data::SortMode::Collation::CaseSensitive:
            state.collation = core::PaneTextCollation::CaseSensitive;
            break;
        default:
            state.collation = core::PaneTextCollation::Unknown;
            break;
    }

    switch( _sort_mode.sort ) {
        case LegacyMode::SortNoSort:
            state.key = Key::Unsorted;
            state.direction = Direction::None;
            break;
        case LegacyMode::SortByRawCName:
            state.key = Key::RawName;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByName:
            state.key = Key::Name;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByNameRev:
            state.key = Key::Name;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortByExt:
            state.key = Key::Extension;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByExtRev:
            state.key = Key::Extension;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortBySize:
            state.key = Key::Size;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortBySizeRev:
            state.key = Key::Size;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByModTime:
            state.key = Key::ModifiedTime;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortByModTimeRev:
            state.key = Key::ModifiedTime;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByBirthTime:
            state.key = Key::CreatedTime;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortByBirthTimeRev:
            state.key = Key::CreatedTime;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByAddTime:
            state.key = Key::AddedTime;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortByAddTimeRev:
            state.key = Key::AddedTime;
            state.direction = Direction::Ascending;
            break;
        case LegacyMode::SortByAccessTime:
            state.key = Key::AccessedTime;
            state.direction = Direction::Descending;
            break;
        case LegacyMode::SortByAccessTimeRev:
            state.key = Key::AccessedTime;
            state.direction = Direction::Ascending;
            break;
        default:
            state.key = Key::Unknown;
            state.direction = Direction::None;
            break;
    }
    return state;
}

std::optional<data::SortMode> RestorePanelSortMode(const core::PaneSortState &_sort_state) noexcept
{
    using Collation = core::PaneTextCollation;
    using Direction = core::PaneSortDirection;
    using Key = core::PaneSortKey;
    using LegacyCollation = data::SortMode::Collation;
    using LegacyMode = data::SortMode::Mode;

    data::SortMode result;
    result.sep_dirs = _sort_state.separates_directories;
    result.extensionless_dirs = _sort_state.extensionless_directories;
    switch( _sort_state.collation ) {
        case Collation::Natural:
            result.collation = LegacyCollation::Natural;
            break;
        case Collation::CaseInsensitive:
            result.collation = LegacyCollation::CaseInsensitive;
            break;
        case Collation::CaseSensitive:
            result.collation = LegacyCollation::CaseSensitive;
            break;
        case Collation::Unknown:
        default:
            return std::nullopt;
    }

    switch( _sort_state.key ) {
        case Key::Unsorted:
            if( _sort_state.direction != Direction::None )
                return std::nullopt;
            result.sort = LegacyMode::SortNoSort;
            break;
        case Key::RawName:
            if( _sort_state.direction != Direction::Ascending )
                return std::nullopt;
            result.sort = LegacyMode::SortByRawCName;
            break;
        case Key::Name:
            if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByName;
            else if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByNameRev;
            else
                return std::nullopt;
            break;
        case Key::Extension:
            if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByExt;
            else if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByExtRev;
            else
                return std::nullopt;
            break;
        case Key::Size:
            if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortBySize;
            else if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortBySizeRev;
            else
                return std::nullopt;
            break;
        case Key::ModifiedTime:
            if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByModTime;
            else if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByModTimeRev;
            else
                return std::nullopt;
            break;
        case Key::CreatedTime:
            if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByBirthTime;
            else if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByBirthTimeRev;
            else
                return std::nullopt;
            break;
        case Key::AddedTime:
            if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByAddTime;
            else if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByAddTimeRev;
            else
                return std::nullopt;
            break;
        case Key::AccessedTime:
            if( _sort_state.direction == Direction::Descending )
                result.sort = LegacyMode::SortByAccessTime;
            else if( _sort_state.direction == Direction::Ascending )
                result.sort = LegacyMode::SortByAccessTimeRev;
            else
                return std::nullopt;
            break;
        case Key::Unknown:
        default:
            return std::nullopt;
    }
    return result;
}

core::PaneGroupingState ProjectPaneGroupingState(const data::SortMode _sort_mode,
                                                 const bool _enabled) noexcept
{
    if( !_enabled )
        return {};

    using LegacyMode = data::SortMode::Mode;
    using Key = core::PaneGroupingKey;

    core::PaneGroupingState state{.enabled = true};
    switch( _sort_mode.sort ) {
        case LegacyMode::SortNoSort:
        case LegacyMode::SortByRawCName:
        case LegacyMode::SortByName:
        case LegacyMode::SortByNameRev:
            state.key = Key::Name;
            break;
        case LegacyMode::SortByExt:
        case LegacyMode::SortByExtRev:
            state.key = Key::Extension;
            break;
        case LegacyMode::SortBySize:
        case LegacyMode::SortBySizeRev:
            state.key = Key::Size;
            break;
        case LegacyMode::SortByModTime:
        case LegacyMode::SortByModTimeRev:
            state.key = Key::ModifiedTime;
            break;
        case LegacyMode::SortByBirthTime:
        case LegacyMode::SortByBirthTimeRev:
            state.key = Key::CreatedTime;
            break;
        case LegacyMode::SortByAddTime:
        case LegacyMode::SortByAddTimeRev:
            state.key = Key::AddedTime;
            break;
        case LegacyMode::SortByAccessTime:
        case LegacyMode::SortByAccessTimeRev:
            state.key = Key::AccessedTime;
            break;
        default:
            state.key = Key::Unknown;
            break;
    }
    return state;
}

core::PaneViewState ProjectPaneViewState(const PanelViewLayout &_layout,
                                         const std::optional<int32_t> _layout_index) noexcept
{
    core::PaneViewState state;
    switch( _layout.type() ) {
        case PanelViewLayout::Type::Brief:
            state.mode = core::PaneViewMode::Icons;
            break;
        case PanelViewLayout::Type::List:
            state.mode = core::PaneViewMode::Details;
            break;
        case PanelViewLayout::Type::Gallery:
            state.mode = core::PaneViewMode::Gallery;
            break;
        case PanelViewLayout::Type::Disabled:
            return state;
        default:
            return state;
    }
    if( _layout_index && *_layout_index >= 0 )
        state.layout_index = _layout_index;
    return state;
}

namespace {

core::PaneSelectedItems MaterializeSelectedItems(const data::Model &_data,
                                                 const VFSListingPtr &_listing)
{
    if( !_data.IsLoaded() )
        return {};

    core::PaneSelectedItems::Storage items;
    // SelectedEntriesSorted is already in deterministic display/sort order.
    for( VFSListingItem selected_item : _data.SelectedEntriesSorted() ) {
        if( selected_item && selected_item.Listing() == _listing &&
            selected_item.Index() < _listing->Count() ) {
            const int selected_position = _data.SortPositionOfEntry(selected_item);
            if( selected_position >= 0 &&
                _data.EntryAtSortPosition(selected_position) == selected_item ) {
                items.emplace_back(std::move(selected_item));
            }
        }
    }
    return core::PaneSelectedItems{std::move(items)};
}

core::PaneState ProjectPaneStateWithSelection(const data::Model &_data,
                                              const unsigned long _location_generation,
                                              VFSListingItem _focused_item,
                                              core::PaneSelectedItems _selected_items,
                                              core::PaneGroupingState _grouping_state,
                                              core::PaneViewState _view_state,
                                              core::PaneHistoryAvailability _history_availability,
                                              std::optional<uint64_t> _current_history_entry_id)
{
    core::PaneState state;
    state.location_generation = _location_generation;
    state.listing = _data.ListingPtr();
    state.sort_state = ProjectPaneSortState(_data.SortMode());
    state.grouping_state = _grouping_state;
    state.view_state = _view_state;
    state.history_availability = _history_availability;
    state.current_history_entry_id = _current_history_entry_id;
    state.shows_hidden_files = _data.HardFiltering().show_hidden;

    if( !_data.IsLoaded() )
        return state;

    state.load_phase = core::PaneLoadPhase::Loaded;
    const VFSListing &listing = _data.Listing();
    state.is_uniform = listing.IsUniform();

    const data::Statistics &statistics = _data.Stats();
    state.item_count = statistics.total_entries_amount;
    state.selected_bytes = statistics.bytes_in_selected_entries;
    if( _selected_items.size() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) )
        throw std::overflow_error("Pane selection exceeds the PaneState count range");
    state.selected_items = std::move(_selected_items);
    state.selected_count = static_cast<int32_t>(state.selected_items.size());

    if( _focused_item && _focused_item.Listing() == state.listing &&
        _focused_item.Index() < state.listing->Count() ) {
        const int focused_position = _data.SortPositionOfEntry(_focused_item);
        if( focused_position >= 0 && _data.EntryAtSortPosition(focused_position) == _focused_item )
            state.focused_item = std::move(_focused_item);
    }

    if( state.is_uniform ) {
        state.host = listing.Host();
        state.path = _data.DirectoryPathWithTrailingSlash();
        state.display_title = _data.VerboseDirectoryFullPath();
    }
    else {
        state.display_title = listing.Title();
    }

    return state;
}

} // namespace

core::PaneState ProjectPaneState(const data::Model &_data,
                                 const unsigned long _location_generation,
                                 VFSListingItem _focused_item,
                                 core::PaneGroupingState _grouping_state,
                                 core::PaneViewState _view_state,
                                 core::PaneHistoryAvailability _history_availability,
                                 std::optional<uint64_t> _current_history_entry_id)
{
    const VFSListingPtr listing = _data.ListingPtr();
    return ProjectPaneStateWithSelection(
        _data,
        _location_generation,
        std::move(_focused_item),
        MaterializeSelectedItems(_data, listing),
        _grouping_state,
        _view_state,
        _history_availability,
        _current_history_entry_id);
}

core::PaneState ProjectPaneState(const data::Model &_data,
                                 const unsigned long _location_generation,
                                 VFSListingItem _focused_item,
                                 core::PaneSelectedItems _selected_items,
                                 core::PaneGroupingState _grouping_state,
                                 core::PaneViewState _view_state,
                                 core::PaneHistoryAvailability _history_availability,
                                 std::optional<uint64_t> _current_history_entry_id)
{
    return ProjectPaneStateWithSelection(
        _data,
        _location_generation,
        std::move(_focused_item),
        std::move(_selected_items),
        _grouping_state,
        _view_state,
        _history_availability,
        _current_history_entry_id);
}

class PanelControllerPaneStoreAdapter::Impl : public std::enable_shared_from_this<Impl>
{
public:
    static std::shared_ptr<Impl> Make(PanelController *_controller)
    {
        dispatch_assert_main_queue();
        if( _controller == nil )
            throw std::invalid_argument("PanelControllerPaneStoreAdapter requires a controller");
        PanelView *const view = _controller.view;
        if( view == nil )
            throw std::invalid_argument("PanelControllerPaneStoreAdapter requires a controller view");

        auto bridge = std::shared_ptr<Impl>(new Impl(_controller, view, _controller.paneId));
        bridge->StartObservingLifecycle();
        bridge->StartObserving(view);
        return bridge;
    }

    ~Impl()
    {
        dispatch_assert_main_queue();
        m_LifecycleObservation = {};
        if( m_ContextObservation != nil )
            [NSNotificationCenter.defaultCenter removeObserver:m_ContextObservation];
    }

    core::PaneStoreAdapter &Store() noexcept { return m_Store; }
    const core::PaneStoreAdapter &Store() const noexcept { return m_Store; }

private:
    Impl(PanelController *_controller, PanelView *_view, const core::PaneId _pane_id) :
        m_Controller(_controller), m_View(_view), m_Store(_pane_id, [this] { return ReadState(); })
    {
        dispatch_assert_main_queue();
    }

    core::PaneState ReadState() const
    {
        dispatch_assert_main_queue();
        const VFSListingItem focused_item = m_SuppressFocusedItem ? VFSListingItem{} : m_View.item;
        const data::Model &model = m_Controller.data;
        const core::PaneGroupingState grouping_state =
            ProjectPaneGroupingState(model.SortMode(), m_View.explorerDetailsGroupingEnabled);
        PanelViewLayout active_layout;
        active_layout.layout = [m_View presentationLayout];
        std::optional<int32_t> layout_index;
        const int controller_layout_index = m_Controller.layoutIndex;
        if( controller_layout_index >= 0 ) {
            const auto configured_layout = m_Controller.layoutStorage.GetLayout(controller_layout_index);
            if( configured_layout && !configured_layout->is_disabled() )
                layout_index = controller_layout_index;
        }
        const core::PaneViewState view_state = ProjectPaneViewState(active_layout, layout_index);
        const History::NavigationState navigation_state = m_Controller.history.GetNavigationState();
        const core::PaneHistoryAvailability history_availability{
            .can_go_back = navigation_state.availability.can_go_back,
            .can_go_forward = navigation_state.availability.can_go_forward,
        };
        const VFSListingPtr listing = model.ListingPtr();
        const uint64_t selection_generation = model.SelectionProjectionGeneration();
        if( !m_HasSelectionCache || m_SelectionListing != listing ||
            m_SelectionGeneration != selection_generation ) {
            core::PaneSelectedItems materialized = MaterializeSelectedItems(model, listing);
            if( materialized != m_SelectedItems )
                m_SelectedItems = std::move(materialized);
            m_SelectionListing = listing;
            m_SelectionGeneration = selection_generation;
            m_HasSelectionCache = true;
        }
        return ProjectPaneState(
            model,
            m_Controller.dataGeneration,
            focused_item,
            m_SelectedItems,
            grouping_state,
            view_state,
            history_availability,
            navigation_state.current_entry_id);
    }

    void StartObservingLifecycle()
    {
        std::weak_ptr<Impl> weak_self = shared_from_this();
        auto subscription = [m_Controller subscribeToPaneLifecycle:[weak_self](const core::PaneLifecycleEvent &_event) noexcept {
            if( const auto self = weak_self.lock() )
                self->OnLifecycleEvent(_event);
        }];
        m_LifecycleObservation = std::move(subscription.observation);
        if( subscription.retained_failure ) {
            if( !subscription.seed_request || subscription.checkpoint_sequence == 0 )
                throw std::logic_error("PanelControllerPaneStoreAdapter received an incomplete failure replay");
            const auto result = m_Store.SeedRetainedLifecycleFailure(
                std::move(*subscription.seed_request),
                *subscription.retained_failure,
                subscription.checkpoint_sequence);
            if( result.status != core::PaneLifecycleReducerStatus::Applied &&
                result.status != core::PaneLifecycleReducerStatus::NoVisibleChange )
                throw std::logic_error("PanelControllerPaneStoreAdapter could not replay lifecycle failure");
        }
        else if( subscription.seed_request ) {
            const auto result = m_Store.SeedActiveLifecycle(std::move(*subscription.seed_request));
            if( result.status != core::PaneLifecycleReducerStatus::Applied &&
                result.status != core::PaneLifecycleReducerStatus::NoVisibleChange )
                throw std::logic_error("PanelControllerPaneStoreAdapter could not seed active lifecycle");
        }
    }

    void OnLifecycleEvent(const core::PaneLifecycleEvent &_event) noexcept
    {
        dispatch_assert_main_queue();
        const bool previous_suppression = m_SuppressFocusedItem;
        if( std::holds_alternative<core::PaneLifecycleCommitted>(_event.payload) )
            m_SuppressFocusedItem = true;
        const auto restore_suppression = at_scope_end([this, previous_suppression] {
            m_SuppressFocusedItem = previous_suppression;
        });
        try {
            const auto result = m_Store.ApplyLifecycleEvent(_event);
            switch( result.status ) {
                case core::PaneLifecycleReducerStatus::Applied:
                case core::PaneLifecycleReducerStatus::NoVisibleChange:
                case core::PaneLifecycleReducerStatus::StaleSequence:
                    break;
                case core::PaneLifecycleReducerStatus::CommitProjectionMismatch:
                    // The reducer consumed the event and fail-closed the public state. Keep the
                    // stream alive while preserving a diagnostic for the projection defect.
                    Log::Error("Pane lifecycle commit projection mismatch at event {}",
                               _event.event_sequence);
                    break;
                case core::PaneLifecycleReducerStatus::WrongPane:
                case core::PaneLifecycleReducerStatus::SequenceGap:
                case core::PaneLifecycleReducerStatus::InvalidTransition:
                case core::PaneLifecycleReducerStatus::InvalidCommittedProjection:
                case core::PaneLifecycleReducerStatus::StaleCommittedProjection:
                    Log::Error("Pane lifecycle reduction rejected event {} with status {}",
                               _event.event_sequence,
                               static_cast<int>(result.status));
                    // These statuses leave the reducer cursor behind the producer. Continuing
                    // would silently wedge every subsequent event, so the boundary is fail-stop.
                    std::terminate();
            }
        } catch( const std::exception &_exception ) {
            Log::Error("Pane lifecycle reduction failed: {}", _exception.what());
            std::terminate();
        } catch( ... ) {
            Log::Error("Pane lifecycle reduction failed with an unknown exception");
            std::terminate();
        }
    }

    void StartObserving(PanelView *_view)
    {
        std::weak_ptr<Impl> weak_self = shared_from_this();
        m_ContextObservation =
            [NSNotificationCenter.defaultCenter addObserverForName:NCPanelViewContextDidChangeNotification
                                                         object:_view
                                                              queue:nil
                                                         usingBlock:^(__unused NSNotification *_notification) {
                                                           if( const auto self = weak_self.lock() )
                                                               self->m_Store.ScheduleRebuild();
                                                         }];
    }

    PanelController *m_Controller;
    PanelView *m_View;
    // The model commit callback runs before PanelView restores its cursor. Its exact projection
    // therefore clears focus; the deferred context rebuild samples the restored live item later.
    bool m_SuppressFocusedItem = false;
    mutable bool m_HasSelectionCache = false;
    mutable VFSListingPtr m_SelectionListing;
    mutable uint64_t m_SelectionGeneration = 0;
    mutable core::PaneSelectedItems m_SelectedItems;
    core::PaneStoreAdapter m_Store;
    core::PaneLifecycleProducer::ObservationTicket m_LifecycleObservation;
    id m_ContextObservation = nil;
};

PanelControllerPaneStoreAdapter::PanelControllerPaneStoreAdapter(PanelController *_controller) :
    m_Impl(Impl::Make(_controller))
{
}

PanelControllerPaneStoreAdapter::~PanelControllerPaneStoreAdapter() = default;

core::PaneStoreAdapter &PanelControllerPaneStoreAdapter::Store() noexcept
{
    return m_Impl->Store();
}

const core::PaneStoreAdapter &PanelControllerPaneStoreAdapter::Store() const noexcept
{
    return m_Impl->Store();
}

} // namespace nc::panel
