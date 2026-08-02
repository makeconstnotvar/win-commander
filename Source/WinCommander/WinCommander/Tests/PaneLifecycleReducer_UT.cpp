// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Pane/PaneLifecycleReducer.h>
#include <Base/dispatch_cpp.h>
#include <VFS/Host.h>
#include <VFS/VFSListing.h>
#include <VFS/VFSListingInput.h>

#include <array>
#include <dirent.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using namespace nc::core;

PaneState EmptyState(const unsigned long _generation = 0)
{
    PaneState state;
    state.location_generation = _generation;
    state.listing = VFSListing::EmptyListing();
    return state;
}

PaneState LoadedState(const unsigned long _generation = 7,
                      std::string _path = "/committed/",
                      VFSListingPtr _listing = VFSListing::EmptyListing())
{
    PaneState state;
    state.location_generation = _generation;
    state.load_phase = PaneLoadPhase::Loaded;
    state.is_uniform = true;
    state.path = std::move(_path);
    state.display_title = state.path;
    state.host = VFSHost::DummyHost();
    state.listing = std::move(_listing);
    state.item_count = 3;
    state.sort_state.key = PaneSortKey::Name;
    state.sort_state.direction = PaneSortDirection::Ascending;
    state.sort_state.collation = PaneTextCollation::CaseInsensitive;
    state.view_state.mode = PaneViewMode::Details;
    return state;
}

VFSListingPtr DistinctEmptyListing()
{
    nc::vfs::ListingInput input;
    input.title = "Refreshed listing";
    input.hosts.insert(0, VFSHost::DummyHost());
    input.directories.insert(0, "/committed/");
    return VFSListing::Build(std::move(input));
}

VFSListingPtr SingleItemListing()
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/committed/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.filenames.emplace_back("focused.txt");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

void SelectOnlyItem(PaneState &_state, const int64_t _bytes = 42)
{
    const auto listing = SingleItemListing();
    _state.listing = listing;
    _state.item_count = 1;
    _state.selected_items = PaneSelectedItems{{listing->Item(0)}};
    _state.selected_count = 1;
    _state.selected_bytes = _bytes;
}

PaneRequestDescriptor Navigation(std::string _path = "/target/")
{
    return PaneRequestDescriptor{
        .kind = PaneRequestKind::Navigation,
        .target = PaneRequestLocation{.host = VFSHost::DummyHost(), .path = std::move(_path)},
        .initiated_by_user = true,
    };
}

PaneRequestDescriptor Refresh()
{
    return PaneRequestDescriptor{.kind = PaneRequestKind::Refresh};
}

FileManagerError Failure(std::string _technical = "failure")
{
    return FileManagerError{
        .code = {.domain = "PaneLifecycleReducerTest", .value = 7},
        .category = FileManagerErrorCategory::NetworkError,
        .severity = FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.test",
        .user_message = "Could not load the folder.",
        .technical_message = std::move(_technical),
        .original_error = nc::Error{"PaneLifecycleReducerTest", 7},
    };
}

template <class Payload>
PaneLifecycleEvent Event(const PaneId _pane,
                         const uint64_t _sequence,
                         const PaneRequestId _request,
                         PaneRequestDescriptor _descriptor,
                         Payload _payload)
{
    return PaneLifecycleEvent{
        .pane_id = _pane,
        .request_id = _request,
        .event_sequence = _sequence,
        .descriptor = std::move(_descriptor),
        .payload = PaneLifecycleEventPayload{std::move(_payload)},
    };
}

} // namespace

#define PREFIX "nc::core::PaneLifecycleReducer "

TEST_CASE(PREFIX "requires a committed baseline and reports its pane identity")
{
    PaneLifecycleReducer reducer(PaneId{41}, EmptyState(3));
    CHECK(reducer.Pane() == PaneId{41});
    CHECK(reducer.State() == EmptyState(3));

    PaneState loading = EmptyState();
    loading.load_phase = PaneLoadPhase::Loading;
    CHECK_THROWS_AS(PaneLifecycleReducer(PaneId{1}, loading), std::invalid_argument);

    PaneState erroneous = LoadedState();
    erroneous.visible_error = Failure();
    CHECK_THROWS_AS(PaneLifecycleReducer(PaneId{1}, erroneous), std::invalid_argument);
}

TEST_CASE(PREFIX "enforces sort-state invariants before committed publication")
{
    struct InvalidSortCase {
        const char *name;
        PaneSortState sort_state;
        bool empty_projection;
    };

    const std::array invalid_cases{
        InvalidSortCase{
            "invalid key enum",
            PaneSortState{
                .key = static_cast<PaneSortKey>(255),
                .direction = PaneSortDirection::Ascending,
                .collation = PaneTextCollation::CaseInsensitive,
            },
            false,
        },
        InvalidSortCase{
            "invalid direction enum",
            PaneSortState{
                .key = PaneSortKey::Name,
                .direction = static_cast<PaneSortDirection>(255),
                .collation = PaneTextCollation::CaseInsensitive,
            },
            false,
        },
        InvalidSortCase{
            "invalid collation enum",
            PaneSortState{
                .key = PaneSortKey::Name,
                .direction = PaneSortDirection::Ascending,
                .collation = static_cast<PaneTextCollation>(255),
            },
            false,
        },
        InvalidSortCase{
            "loaded unknown key",
            PaneSortState{
                .key = PaneSortKey::Unknown,
                .direction = PaneSortDirection::None,
                .collation = PaneTextCollation::CaseInsensitive,
            },
            false,
        },
        InvalidSortCase{
            "loaded unknown collation",
            PaneSortState{
                .key = PaneSortKey::Name,
                .direction = PaneSortDirection::Ascending,
                .collation = PaneTextCollation::Unknown,
            },
            false,
        },
        InvalidSortCase{
            "empty unknown key with a direction",
            PaneSortState{
                .key = PaneSortKey::Unknown,
                .direction = PaneSortDirection::Ascending,
                .collation = PaneTextCollation::Unknown,
            },
            true,
        },
        InvalidSortCase{
            "unsorted with a direction",
            PaneSortState{
                .key = PaneSortKey::Unsorted,
                .direction = PaneSortDirection::Ascending,
                .collation = PaneTextCollation::Natural,
            },
            false,
        },
        InvalidSortCase{
            "raw name descending",
            PaneSortState{
                .key = PaneSortKey::RawName,
                .direction = PaneSortDirection::Descending,
                .collation = PaneTextCollation::CaseSensitive,
            },
            false,
        },
        InvalidSortCase{
            "real key without a direction",
            PaneSortState{
                .key = PaneSortKey::Size,
                .direction = PaneSortDirection::None,
                .collation = PaneTextCollation::Natural,
            },
            false,
        },
    };

    const PaneState baseline = LoadedState();
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, PaneState{}));

    for( const InvalidSortCase &test_case : invalid_cases ) {
        CAPTURE(test_case.name);
        PaneState invalid = test_case.empty_projection ? PaneState{} : LoadedState();
        invalid.sort_state = test_case.sort_state;
        CHECK_THROWS_AS(PaneLifecycleReducer(PaneId{1}, invalid), std::invalid_argument);

        PaneLifecycleReducer reducer(PaneId{1}, baseline);
        CHECK(reducer.UpdateCommittedProjection(invalid) ==
              PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidCommittedProjection, false});
        CHECK(reducer.State() == baseline);
    }

    for( const PaneSortState valid : std::array{
             PaneSortState{
                 .key = PaneSortKey::Unsorted,
                 .direction = PaneSortDirection::None,
                 .collation = PaneTextCollation::Natural,
             },
             PaneSortState{
                 .key = PaneSortKey::RawName,
                 .direction = PaneSortDirection::Ascending,
                 .collation = PaneTextCollation::CaseSensitive,
             },
             PaneSortState{
                 .key = PaneSortKey::Size,
                 .direction = PaneSortDirection::Descending,
                 .collation = PaneTextCollation::CaseInsensitive,
             },
         } ) {
        PaneState state = LoadedState();
        state.sort_state = valid;
        CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, state));
    }
}

TEST_CASE(PREFIX "enforces grouping and view invariants before committed publication")
{
    const PaneState baseline = LoadedState();
    auto check_invalid = [&](const PaneState &_invalid) {
        CHECK_THROWS_AS(PaneLifecycleReducer(PaneId{1}, _invalid), std::invalid_argument);
        PaneLifecycleReducer reducer(PaneId{1}, baseline);
        CHECK(reducer.UpdateCommittedProjection(_invalid) ==
              PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidCommittedProjection, false});
        CHECK(reducer.State() == baseline);
    };

    PaneState disabled_with_key = LoadedState();
    disabled_with_key.grouping_state.key = PaneGroupingKey::Name;
    check_invalid(disabled_with_key);

    PaneState grouping_mismatch = LoadedState();
    grouping_mismatch.grouping_state =
        PaneGroupingState{.enabled = true, .key = PaneGroupingKey::Size};
    check_invalid(grouping_mismatch);

    PaneState invalid_grouping_enum = LoadedState();
    invalid_grouping_enum.grouping_state = PaneGroupingState{
        .enabled = true,
        .key = static_cast<PaneGroupingKey>(255),
    };
    check_invalid(invalid_grouping_enum);

    PaneState loaded_unknown_view = LoadedState();
    loaded_unknown_view.view_state = {};
    check_invalid(loaded_unknown_view);

    PaneState invalid_view_enum = LoadedState();
    invalid_view_enum.view_state.mode = static_cast<PaneViewMode>(255);
    check_invalid(invalid_view_enum);

    PaneState negative_layout_index = LoadedState();
    negative_layout_index.view_state.layout_index = -1;
    check_invalid(negative_layout_index);

    PaneState empty_unknown_view_with_index;
    empty_unknown_view_with_index.view_state.layout_index = 0;
    check_invalid(empty_unknown_view_with_index);

    PaneState valid = LoadedState();
    valid.grouping_state = PaneGroupingState{.enabled = true, .key = PaneGroupingKey::Name};
    valid.view_state = PaneViewState{.mode = PaneViewMode::Icons, .layout_index = 0};
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, valid));
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, PaneState{}));
}

TEST_CASE(PREFIX "enforces history identity and availability invariants")
{
    const PaneState baseline = LoadedState();
    const auto check_invalid = [&](const PaneState &_invalid) {
        CHECK_THROWS_AS(PaneLifecycleReducer(PaneId{1}, _invalid), std::invalid_argument);
        PaneLifecycleReducer reducer(PaneId{1}, baseline);
        CHECK(reducer.UpdateCommittedProjection(_invalid) ==
              PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidCommittedProjection, false});
        CHECK(reducer.State() == baseline);
    };

    PaneState back_without_identity = LoadedState();
    back_without_identity.history_availability.can_go_back = true;
    check_invalid(back_without_identity);

    PaneState forward_without_identity = LoadedState();
    forward_without_identity.history_availability.can_go_forward = true;
    check_invalid(forward_without_identity);

    PaneState zero_identity = LoadedState();
    zero_identity.current_history_entry_id = 0;
    check_invalid(zero_identity);

    PaneState single_entry = LoadedState();
    single_entry.current_history_entry_id = 1;
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, single_entry));

    PaneState navigable = LoadedState();
    navigable.current_history_entry_id = 2;
    navigable.history_availability = {.can_go_back = true, .can_go_forward = true};
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, navigable));

    PaneState unloaded_single_entry;
    unloaded_single_entry.current_history_entry_id = 3;
    CHECK_NOTHROW(PaneLifecycleReducer(PaneId{1}, unloaded_single_entry));
}

TEST_CASE(PREFIX "seeds an exact retained failure with a separate observation checkpoint")
{
    const PaneId pane{42};
    const PaneRequestId failed_request{8};
    const auto descriptor = Navigation("/failed/");
    const auto failure = Event(
        pane, 5, failed_request, descriptor, PaneLifecycleFailed{Failure("retained")});
    PaneLifecycleReducer reducer(pane, LoadedState());

    CHECK(reducer.SeedRetainedFailure(
              PaneActiveRequest{failed_request, descriptor}, failure, 7) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(failure.event_sequence == 5);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Failed);
    REQUIRE(reducer.State().visible_error);
    CHECK(reducer.State().visible_error->technical_message == "retained");

    CHECK(reducer.Apply(Event(
              pane, 8, PaneRequestId{9}, Navigation("/next/"), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);

    PaneLifecycleReducer invalid(pane, LoadedState());
    CHECK(invalid.SeedRetainedFailure(
              PaneActiveRequest{failed_request, descriptor}, failure, 4) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
}

TEST_CASE(PREFIX "overlays navigation and commits only an exact post-model projection")
{
    const PaneId pane{1};
    const PaneRequestId request{11};
    const auto descriptor = Navigation();
    const PaneState initial = LoadedState();
    PaneLifecycleReducer reducer(pane, initial);

    const auto started = reducer.Apply(Event(pane, 10, request, descriptor, PaneLifecycleStarted{}));
    CHECK(started == PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);
    CHECK(reducer.State().path == initial.path);
    CHECK(reducer.State().listing == initial.listing);
    CHECK_FALSE(reducer.State().visible_error);

    PaneState selection_change = initial;
    SelectOnlyItem(selection_change, 84);
    CHECK(reducer.UpdateCommittedProjection(selection_change) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);
    CHECK(reducer.State().selected_count == 1);
    CHECK(reducer.State().selected_items.StorageIdentity() ==
          selection_change.selected_items.StorageIdentity());

    PaneState stale = selection_change;
    stale.location_generation = 6;
    stale.path = "/stale/";
    CHECK(reducer.UpdateCommittedProjection(stale) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::StaleCommittedProjection, false});
    CHECK(reducer.State().path == initial.path);

    PaneState committed = LoadedState(8, "/target/");
    SelectOnlyItem(committed, 96);
    const auto committed_event = Event(
        pane,
        11,
        request,
        descriptor,
        PaneLifecycleCommitted{.controller_generation = 8, .listing = committed.listing});
    CHECK(reducer.Apply(committed_event, &committed) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State() == committed);
    CHECK(reducer.State().selected_items.StorageIdentity() == committed.selected_items.StorageIdentity());
    CHECK(reducer.UpdateCommittedProjection(committed) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
}

TEST_CASE(PREFIX "seeds an already active request without inventing an event sequence")
{
    const PaneId pane{12};
    const PaneRequestId request{31};
    const auto descriptor = Navigation("/seeded/");
    PaneLifecycleReducer reducer(pane, LoadedState());

    CHECK(reducer.SeedActive(PaneActiveRequest{request, descriptor}) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);

    const PaneState loading = reducer.State();
    CHECK(reducer.SeedActive(PaneActiveRequest{PaneRequestId{32}, Refresh()}) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.State() == loading);

    PaneState committed = LoadedState(8, "/seeded/");
    const auto terminal = Event(
        pane,
        93,
        request,
        descriptor,
        PaneLifecycleCommitted{.controller_generation = 8, .listing = committed.listing});
    CHECK(reducer.Apply(terminal, &committed) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State() == committed);

    CHECK(reducer.SeedActive(PaneActiveRequest{PaneRequestId{33}, Refresh()}) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.State() == committed);

    PaneLifecycleReducer refresh_reducer(PaneId{13}, LoadedState());
    CHECK(refresh_reducer.SeedActive(PaneActiveRequest{}) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(refresh_reducer.SeedActive(PaneActiveRequest{PaneRequestId{41}, Refresh()}).state_changed);
    CHECK(refresh_reducer.State().load_phase == PaneLoadPhase::Refreshing);
    CHECK(refresh_reducer.Apply(Event(
              PaneId{13},
              117,
              PaneRequestId{41},
              Refresh(),
              PaneLifecycleCancelled{PaneCancellationReason::User}))
              .state_changed);
    CHECK(refresh_reducer.State().load_phase == PaneLoadPhase::Loaded);
}

TEST_CASE(PREFIX "keeps refresh content authoritative when refresh fails")
{
    const PaneId pane{2};
    const PaneRequestId request{1};
    const PaneState initial = LoadedState();
    PaneLifecycleReducer reducer(pane, initial);

    CHECK(reducer.Apply(Event(pane, 1, request, Refresh(), PaneLifecycleStarted{})).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Refreshing);
    CHECK(reducer.State().listing == initial.listing);

    const FileManagerError error = Failure("refresh failed");
    CHECK(reducer.Apply(Event(pane, 2, request, Refresh(), PaneLifecycleFailed{error})).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loaded);
    REQUIRE(reducer.State().visible_error);
    CHECK(*reducer.State().visible_error == error);
    CHECK(reducer.State().path == initial.path);

    PaneState counts = initial;
    SelectOnlyItem(counts);
    CHECK(reducer.UpdateCommittedProjection(counts).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loaded);
    CHECK(reducer.State().visible_error == error);
    CHECK(reducer.State().selected_count == 1);

    PaneState external_commit = LoadedState(8, "/external/");
    CHECK(reducer.UpdateCommittedProjection(external_commit).state_changed);
    CHECK(reducer.State() == external_commit);
    CHECK_FALSE(reducer.State().visible_error);
}

TEST_CASE(PREFIX "commits refreshed content without advancing the location generation")
{
    const PaneId pane{21};
    const PaneRequestId request{1};
    const auto descriptor = Refresh();
    const PaneState initial = LoadedState();
    PaneLifecycleReducer reducer(pane, initial);

    CHECK(reducer.Apply(Event(pane, 1, request, descriptor, PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Refreshing);
    CHECK(reducer.State().location_generation == initial.location_generation);
    CHECK(reducer.State().listing == initial.listing);

    PaneState committed = LoadedState(
        initial.location_generation, initial.path, DistinctEmptyListing());
    CHECK(reducer.Apply(
              Event(pane,
                    2,
                    request,
                    descriptor,
                    PaneLifecycleCommitted{
                        .controller_generation = committed.location_generation,
                        .listing = committed.listing,
                    }),
              &committed) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State() == committed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loaded);
    CHECK(reducer.State().location_generation == initial.location_generation);
    CHECK(reducer.State().listing != initial.listing);
    CHECK_FALSE(reducer.State().visible_error);
}

TEST_CASE(PREFIX "makes navigation failure visible until a newer accepted intent")
{
    const PaneId pane{3};
    const PaneRequestId first{1};
    const PaneRequestId rejected{2};
    const PaneRequestId next{3};
    PaneLifecycleReducer reducer(pane, LoadedState());

    CHECK(reducer.Apply(Event(pane, 1, first, Navigation(), PaneLifecycleStarted{})).state_changed);
    const FileManagerError error = Failure("navigation failed");
    CHECK(reducer.Apply(Event(pane, 2, first, Navigation(), PaneLifecycleFailed{error})).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Failed);
    REQUIRE(reducer.State().visible_error);
    CHECK(*reducer.State().visible_error == error);

    const FileManagerError admission_error = Failure("probe failed");
    const PaneState before_rejection = reducer.State();
    CHECK(reducer.Apply(Event(
              pane,
              3,
              rejected,
              Navigation("/rejected/"),
              PaneLifecycleRejected{.reason = PaneRejectionReason::Unavailable,
                                    .admission_error = admission_error})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
    CHECK(reducer.State() == before_rejection);

    CHECK(reducer.Apply(Event(pane, 4, next, Navigation("/next/"), PaneLifecycleStarted{})).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);
    CHECK_FALSE(reducer.State().visible_error);
}

TEST_CASE(PREFIX "treats every cancellation reason as a non-error terminal")
{
    constexpr auto reasons = std::to_array<PaneCancellationReason>({
        PaneCancellationReason::User,
        PaneCancellationReason::QueueStopped,
        PaneCancellationReason::ProducerShutdown,
        PaneCancellationReason::InternalAbort,
    });

    for( const auto reason : reasons ) {
        CAPTURE(reason);
        const PaneId pane{4};
        const PaneRequestId request{1};
        const PaneState initial = LoadedState();
        PaneLifecycleReducer reducer(pane, initial);
        REQUIRE(reducer.Apply(Event(pane, 20, request, Navigation(), PaneLifecycleStarted{})).state_changed);

        CHECK(reducer.Apply(Event(pane, 21, request, Navigation(), PaneLifecycleCancelled{reason})) ==
              PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
        CHECK(reducer.State() == initial);
        CHECK_FALSE(reducer.State().visible_error);

        CHECK(reducer.Apply(Event(
                  pane,
                  21,
                  request,
                  Navigation(),
                  PaneLifecycleCommitted{.controller_generation = 8, .listing = initial.listing})) ==
              PaneLifecycleReducerResult{PaneLifecycleReducerStatus::StaleSequence, false});
    }
}

TEST_CASE(PREFIX "supersedes without publishing an intermediate committed phase")
{
    const PaneId pane{5};
    const PaneRequestId first{1};
    const PaneRequestId second{2};
    PaneLifecycleReducer reducer(pane, LoadedState());

    REQUIRE(reducer.Apply(Event(pane, 1, first, Refresh(), PaneLifecycleStarted{})).state_changed);
    const PaneState refreshing = reducer.State();
    CHECK(refreshing.load_phase == PaneLoadPhase::Refreshing);

    CHECK(reducer.Apply(Event(
              pane,
              2,
              first,
              Refresh(),
              PaneLifecycleSuperseded{.replacement = second})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
    CHECK(reducer.State() == refreshing);

    CHECK(reducer.Apply(Event(pane, 3, second, Navigation("/replacement/"), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);

    CHECK(reducer.Apply(Event(
              pane,
              4,
              first,
              Refresh(),
              PaneLifecycleCommitted{.controller_generation = 8, .listing = VFSListing::EmptyListing()})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.Apply(Event(
              pane,
              4,
              second,
              Navigation("/replacement/"),
              PaneLifecycleCancelled{PaneCancellationReason::User})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::Applied, true});
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loaded);
}

TEST_CASE(PREFIX "same-kind replacement changes no public state")
{
    const PaneId pane{6};
    PaneLifecycleReducer reducer(pane, LoadedState());
    REQUIRE(reducer.Apply(Event(pane, 1, PaneRequestId{1}, Navigation(), PaneLifecycleStarted{})).state_changed);
    const PaneState loading = reducer.State();

    CHECK(reducer.Apply(Event(
              pane,
              2,
              PaneRequestId{1},
              Navigation(),
              PaneLifecycleSuperseded{.replacement = PaneRequestId{2}})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});

    PaneState context_change = LoadedState();
    SelectOnlyItem(context_change);
    CHECK(reducer.UpdateCommittedProjection(context_change).state_changed);
    CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);
    CHECK(reducer.State().selected_count == 1);
    const PaneState loading_with_context = reducer.State();

    CHECK(reducer.Apply(Event(
              pane,
              3,
              PaneRequestId{2},
              Navigation("/replacement/"),
              PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
    CHECK(reducer.State() == loading_with_context);
}

TEST_CASE(PREFIX "rejections do not disturb idle or active accepted state")
{
    const PaneId pane{7};
    const PaneState initial = LoadedState();
    PaneLifecycleReducer reducer(pane, initial);

    CHECK(reducer.Apply(Event(
              pane,
              5,
              PaneRequestId{1},
              Navigation("/invalid/"),
              PaneLifecycleRejected{.reason = PaneRejectionReason::InvalidRequest})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
    CHECK(reducer.State() == initial);

    REQUIRE(reducer.Apply(Event(pane, 6, PaneRequestId{2}, Navigation(), PaneLifecycleStarted{})).state_changed);
    const PaneState loading = reducer.State();
    CHECK(reducer.Apply(Event(
              pane,
              7,
              PaneRequestId{3},
              Navigation("/busy/"),
              PaneLifecycleRejected{.reason = PaneRejectionReason::Busy,
                                    .conflicting_request = PaneRequestId{2}})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::NoVisibleChange, false});
    CHECK(reducer.State() == loading);
}

TEST_CASE(PREFIX "enforces pane request and contiguous sequence identity")
{
    const PaneId pane{8};
    const PaneRequestId request{1};
    PaneLifecycleReducer reducer(pane, EmptyState());

    CHECK(reducer.Apply(Event(PaneId{9}, 10, request, Navigation(), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::WrongPane, false});
    CHECK(reducer.Apply(Event(pane, 0, request, Navigation(), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::SequenceGap, false});
    CHECK(reducer.Apply(Event(pane, 10, request, Navigation(), PaneLifecycleStarted{})).state_changed);
    CHECK(reducer.Apply(Event(pane, 10, request, Navigation(), PaneLifecycleCancelled{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::StaleSequence, false});
    CHECK(reducer.Apply(Event(pane, 12, request, Navigation(), PaneLifecycleCancelled{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::SequenceGap, false});
    CHECK(reducer.Apply(Event(pane, 11, PaneRequestId{2}, Navigation(), PaneLifecycleCancelled{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.Apply(Event(pane, 11, request, Navigation(), PaneLifecycleCancelled{})).state_changed);
}

TEST_CASE(PREFIX "fails closed on every committed projection mismatch")
{
    struct Mismatch {
        PaneState projection;
        bool provide_projection = true;
        uint64_t event_generation = 8;
        VFSListingPtr event_listing = VFSListing::EmptyListing();
    };

    PaneState invalid_phase = LoadedState(8, "/new/");
    invalid_phase.load_phase = PaneLoadPhase::Loading;
    PaneState wrong_generation = LoadedState(9, "/new/");
    PaneState wrong_listing = LoadedState(8, "/new/", nullptr);
    PaneState stale_projection = LoadedState(6, "/new/");
    const auto focused_listing = SingleItemListing();
    PaneState foreign_focus = LoadedState(8, "/new/", focused_listing);
    const auto foreign_listing = SingleItemListing();
    foreign_focus.focused_item = foreign_listing->Item(0);
    PaneState out_of_range_focus = LoadedState(8, "/new/", focused_listing);
    out_of_range_focus.focused_item =
        VFSListingItem{focused_listing, focused_listing->Count()};
    PaneState empty_with_focus = EmptyState(8);
    empty_with_focus.listing = focused_listing;
    empty_with_focus.focused_item = focused_listing->Item(0);
    PaneState foreign_selection = LoadedState(8, "/new/", focused_listing);
    foreign_selection.selected_items = PaneSelectedItems{{foreign_listing->Item(0)}};
    foreign_selection.selected_count = 1;
    foreign_selection.selected_bytes = 1;
    PaneState out_of_range_selection = LoadedState(8, "/new/", focused_listing);
    out_of_range_selection.selected_items = PaneSelectedItems{{
        VFSListingItem{focused_listing, focused_listing->Count()}}};
    out_of_range_selection.selected_count = 1;
    out_of_range_selection.selected_bytes = 1;
    PaneState selection_count_mismatch = LoadedState(8, "/new/", focused_listing);
    selection_count_mismatch.selected_items = PaneSelectedItems{{focused_listing->Item(0)}};
    selection_count_mismatch.selected_count = 0;
    selection_count_mismatch.selected_bytes = 1;
    PaneState duplicate_selection = LoadedState(8, "/new/", focused_listing);
    duplicate_selection.selected_items =
        PaneSelectedItems{{focused_listing->Item(0), focused_listing->Item(0)}};
    duplicate_selection.selected_count = 2;
    duplicate_selection.selected_bytes = 2;
    PaneState empty_with_selection = EmptyState(8);
    empty_with_selection.listing = focused_listing;
    empty_with_selection.selected_items = PaneSelectedItems{{focused_listing->Item(0)}};
    empty_with_selection.selected_count = 1;
    empty_with_selection.selected_bytes = 1;
    PaneState empty_with_count = EmptyState(8);
    empty_with_count.selected_count = 1;
    PaneState empty_with_bytes = EmptyState(8);
    empty_with_bytes.selected_bytes = 1;
    PaneState negative_item_count = LoadedState(8, "/new/");
    negative_item_count.item_count = -1;
    PaneState empty_selection_with_bytes = LoadedState(8, "/new/");
    empty_selection_with_bytes.selected_bytes = 1;
    PaneState selection_exceeds_item_count = LoadedState(8, "/new/", focused_listing);
    selection_exceeds_item_count.item_count = 0;
    selection_exceeds_item_count.selected_items = PaneSelectedItems{{focused_listing->Item(0)}};
    selection_exceeds_item_count.selected_count = 1;
    selection_exceeds_item_count.selected_bytes = 1;
    const std::array mismatches = {
        Mismatch{.projection = LoadedState(8, "/new/"), .provide_projection = false},
        Mismatch{.projection = invalid_phase},
        Mismatch{.projection = wrong_generation},
        Mismatch{.projection = wrong_listing},
        Mismatch{.projection = stale_projection, .event_generation = 6},
        Mismatch{.projection = foreign_focus, .event_listing = focused_listing},
        Mismatch{.projection = out_of_range_focus, .event_listing = focused_listing},
        Mismatch{.projection = empty_with_focus, .event_listing = focused_listing},
        Mismatch{.projection = foreign_selection, .event_listing = focused_listing},
        Mismatch{.projection = out_of_range_selection, .event_listing = focused_listing},
        Mismatch{.projection = selection_count_mismatch, .event_listing = focused_listing},
        Mismatch{.projection = duplicate_selection, .event_listing = focused_listing},
        Mismatch{.projection = empty_with_selection, .event_listing = focused_listing},
        Mismatch{.projection = empty_with_count},
        Mismatch{.projection = empty_with_bytes},
        Mismatch{.projection = negative_item_count},
        Mismatch{.projection = empty_selection_with_bytes},
        Mismatch{.projection = selection_exceeds_item_count, .event_listing = focused_listing},
    };

    for( const auto &mismatch : mismatches ) {
        PaneLifecycleReducer reducer(PaneId{9}, LoadedState());
        REQUIRE(reducer.Apply(Event(
                    PaneId{9}, 1, PaneRequestId{1}, Navigation(), PaneLifecycleStarted{}))
                    .state_changed);
        const auto committed = Event(
            PaneId{9},
            2,
            PaneRequestId{1},
            Navigation(),
            PaneLifecycleCommitted{
                .controller_generation = mismatch.event_generation,
                .listing = mismatch.event_listing,
            });
        const PaneState *projection = mismatch.provide_projection ? &mismatch.projection : nullptr;
        const auto result = reducer.Apply(committed, projection);
        CHECK(result.status == PaneLifecycleReducerStatus::CommitProjectionMismatch);
        CHECK(result.state_changed);
        CHECK(reducer.State().load_phase == PaneLoadPhase::Failed);
        CHECK_FALSE(reducer.State().visible_error);

        CHECK(reducer.Apply(Event(
                  PaneId{9}, 3, PaneRequestId{2}, Navigation("/retry/"), PaneLifecycleStarted{}))
                  .state_changed);
        CHECK(reducer.State().load_phase == PaneLoadPhase::Loading);
    }
}

TEST_CASE(PREFIX "rejects contradictory counts before reusing a trusted selection payload")
{
    PaneState committed = LoadedState();
    SelectOnlyItem(committed);
    PaneLifecycleReducer reducer(PaneId{11}, committed);

    PaneState malformed = committed;
    REQUIRE(malformed.selected_items.StorageIdentity() ==
            committed.selected_items.StorageIdentity());
    malformed.item_count = 0;

    CHECK(reducer.UpdateCommittedProjection(std::move(malformed)) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidCommittedProjection, false});
    CHECK(reducer.State() == committed);
}

TEST_CASE(PREFIX "rejects invalid committed updates and replacement transitions")
{
    const PaneId pane{10};
    PaneLifecycleReducer reducer(pane, LoadedState());
    PaneState invalid = LoadedState();
    invalid.load_phase = PaneLoadPhase::Failed;
    CHECK(reducer.UpdateCommittedProjection(invalid) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidCommittedProjection, false});

    CHECK(reducer.Apply(Event(pane, 1, PaneRequestId{1}, Navigation(), PaneLifecycleCommitted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    REQUIRE(reducer.Apply(Event(pane, 1, PaneRequestId{1}, Navigation(), PaneLifecycleStarted{})).state_changed);
    CHECK(reducer.Apply(Event(pane, 2, PaneRequestId{2}, Navigation(), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    REQUIRE(reducer.Apply(Event(
                pane,
                2,
                PaneRequestId{1},
                Navigation(),
                PaneLifecycleSuperseded{.replacement = PaneRequestId{2}}))
                .status == PaneLifecycleReducerStatus::NoVisibleChange);
    CHECK(reducer.Apply(Event(
              pane,
              3,
              PaneRequestId{3},
              Navigation(),
              PaneLifecycleRejected{.reason = PaneRejectionReason::Busy})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.Apply(Event(pane, 3, PaneRequestId{3}, Navigation(), PaneLifecycleStarted{})) ==
          PaneLifecycleReducerResult{PaneLifecycleReducerStatus::InvalidTransition, false});
    CHECK(reducer.Apply(Event(pane, 3, PaneRequestId{2}, Refresh(), PaneLifecycleStarted{})).state_changed);
}

TEST_CASE(PREFIX "reduces producer replacement batches without public loading flicker")
{
    REQUIRE(nc::dispatch_is_main_queue());
    const PaneId pane{11};
    PaneState committed = LoadedState();
    PaneLifecycleReducer reducer(pane, committed);
    PaneLifecycleProducer producer(pane);
    std::vector<PaneLoadPhase> changed_phases;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        const PaneState *projection =
            std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ? &committed : nullptr;
        const auto result = reducer.Apply(_event, projection);
        REQUIRE((result.status == PaneLifecycleReducerStatus::Applied ||
                 result.status == PaneLifecycleReducerStatus::NoVisibleChange));
        if( result.state_changed )
            changed_phases.emplace_back(reducer.State().load_phase);
    });

    const auto first = producer.Start(Navigation("/first/"));
    const auto second = producer.SupersedeAndStart(Navigation("/second/"));
    CHECK(producer.Finish(second, PaneLifecycleCancelled{PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);

    CHECK(first == PaneRequestId{1});
    CHECK(second == PaneRequestId{2});
    CHECK(changed_phases == std::vector<PaneLoadPhase>{PaneLoadPhase::Loading, PaneLoadPhase::Loaded});
}

#undef PREFIX
