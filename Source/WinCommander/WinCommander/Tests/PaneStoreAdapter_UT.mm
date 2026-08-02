// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Pane/PaneStoreAdapter.h>
#include <Base/dispatch_cpp.h>
#include <VFS/Host.h>
#include <VFS/VFSListing.h>
#include <VFS/VFSListingInput.h>
#include <CoreFoundation/CoreFoundation.h>
#include <chrono>
#include <dirent.h>
#include <future>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nc::core::PaneId;
using nc::core::PaneLoadPhase;
using nc::core::PaneSnapshot;
using nc::core::PaneState;
using nc::core::PaneStoreAdapter;

nc::core::PaneRequestDescriptor Navigation(std::string _path = "/next/")
{
    return nc::core::PaneRequestDescriptor{
        .kind = nc::core::PaneRequestKind::Navigation,
        .target = nc::core::PaneRequestLocation{.host = VFSHost::DummyHost(), .path = std::move(_path)},
    };
}

nc::core::PaneRequestDescriptor Refresh()
{
    return nc::core::PaneRequestDescriptor{.kind = nc::core::PaneRequestKind::Refresh};
}

nc::core::FileManagerError Failure()
{
    return nc::core::FileManagerError{
        .code = {.domain = "PaneStoreAdapterTest", .value = 1},
        .category = nc::core::FileManagerErrorCategory::NetworkError,
        .severity = nc::core::FileManagerErrorSeverity::RecoverableError,
        .user_message_key = "errors.test",
        .user_message = "The folder could not be refreshed.",
        .technical_message = "Test refresh failure.",
        .original_error = nc::Error{"PaneStoreAdapterTest", 1},
    };
}

template <class Payload>
nc::core::PaneLifecycleEvent Event(const uint64_t _sequence,
                                   const nc::core::PaneRequestId _request,
                                   nc::core::PaneRequestDescriptor _descriptor,
                                   Payload _payload)
{
    return nc::core::PaneLifecycleEvent{
        .pane_id = PaneId{1},
        .request_id = _request,
        .event_sequence = _sequence,
        .descriptor = std::move(_descriptor),
        .payload = nc::core::PaneLifecycleEventPayload{std::move(_payload)},
    };
}

bool RunMainLoopUntil(const std::function<bool()> &_predicate, const std::chrono::milliseconds _timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    while( !_predicate() && std::chrono::steady_clock::now() < deadline )
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    return _predicate();
}

PaneState LoadedState()
{
    PaneState state;
    state.location_generation = 7;
    state.load_phase = PaneLoadPhase::Loaded;
    state.is_uniform = true;
    state.path = "/tmp/";
    state.display_title = "/tmp";
    state.host = VFSHost::DummyHost();
    state.listing = VFSListing::EmptyListing();
    state.item_count = 3;
    state.sort_state.key = nc::core::PaneSortKey::Name;
    state.sort_state.direction = nc::core::PaneSortDirection::Ascending;
    state.sort_state.collation = nc::core::PaneTextCollation::CaseInsensitive;
    state.view_state.mode = nc::core::PaneViewMode::Details;
    return state;
}

VFSListingPtr DistinctEmptyListing()
{
    nc::vfs::ListingInput input;
    input.title = "Distinct empty listing";
    input.hosts.insert(0, VFSHost::DummyHost());
    input.directories.insert(0, "/distinct/");
    return VFSListing::Build(std::move(input));
}

VFSListingPtr FocusListing()
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/focused/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.filenames.emplace_back("focused.txt");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

} // namespace

#define PREFIX "PaneStoreAdapter "

TEST_CASE(PREFIX "publishes the injected initial state without copying engine references")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    int reader_calls = 0;

    PaneStoreAdapter store(PaneId{42}, [&] {
        ++reader_calls;
        return source;
    });

    const PaneSnapshot snapshot = store.Snapshot();
    CHECK(reader_calls == 1);
    CHECK(snapshot.pane_id == PaneId{42});
    CHECK(snapshot.revision == 0);
    CHECK(snapshot.listing_generation == 0);
    CHECK(snapshot.state == source);
    CHECK(snapshot.state.host == source.host);
    CHECK(snapshot.state.listing == source.listing);
}

TEST_CASE(PREFIX "commits load phases and tracks listing identity independently from revision")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source;
    PaneStoreAdapter store(PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    int observations = 0;
    uint64_t observed_revision = 0;
    auto ticket = store.Observe([&](const PaneSnapshot &_snapshot) {
        ++observations;
        observed_revision = _snapshot.revision;
        CHECK(nc::dispatch_is_main_queue());
    });
    REQUIRE(ticket);

    const nc::core::PaneRequestId navigation{1};
    const auto navigation_descriptor = Navigation();
    CHECK(store.ApplyLifecycleEvent(
              Event(1, navigation, navigation_descriptor, nc::core::PaneLifecycleStarted{}))
              .state_changed);
    CHECK(store.Snapshot().revision == 1);
    CHECK(store.Snapshot().listing_generation == 0);

    source = LoadedState();
    CHECK(store.ApplyLifecycleEvent(Event(
              2,
              navigation,
              navigation_descriptor,
              nc::core::PaneLifecycleCommitted{
                  .controller_generation = source.location_generation,
                  .listing = source.listing,
              }))
              .state_changed);
    CHECK(store.Snapshot().revision == 2);
    CHECK(store.Snapshot().listing_generation == 1);

    const nc::core::PaneRequestId refresh{2};
    const auto refresh_descriptor = Refresh();
    CHECK(store.ApplyLifecycleEvent(
              Event(3, refresh, refresh_descriptor, nc::core::PaneLifecycleStarted{}))
              .state_changed);
    CHECK(store.Snapshot().revision == 3);
    CHECK(store.Snapshot().listing_generation == 1);

    CHECK(store.ApplyLifecycleEvent(
              Event(4, refresh, refresh_descriptor, nc::core::PaneLifecycleFailed{Failure()}))
              .state_changed);
    CHECK(store.Snapshot().revision == 4);
    CHECK(store.Snapshot().listing_generation == 1);
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loaded);
    CHECK(store.Snapshot().state.visible_error.has_value());

    source.shows_hidden_files = true;
    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 5);
    CHECK(store.Snapshot().listing_generation == 1);

    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 5);
    CHECK(observations == 5);
    CHECK(observed_revision == 5);

    source = PaneState{};
    source.location_generation = 8;
    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 6);
    CHECK(store.Snapshot().listing_generation == 2);
    CHECK_FALSE(store.Snapshot().state.visible_error);
}

TEST_CASE(PREFIX "publishes focus-only changes without advancing listing generation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    source.listing = FocusListing();
    source.item_count = 1;
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    const auto initial = store.Snapshot();
    CHECK_FALSE(initial.state.focused_item);

    source.focused_item = source.listing->Item(0);
    store.ScheduleRebuild();

    const auto focused = store.Snapshot();
    CHECK(focused.revision == initial.revision + 1);
    CHECK(focused.listing_generation == initial.listing_generation);
    CHECK(focused.state.focused_item == source.focused_item);

    source.focused_item = {};
    store.ScheduleRebuild();

    const auto cleared = store.Snapshot();
    CHECK(cleared.revision == focused.revision + 1);
    CHECK(cleared.listing_generation == focused.listing_generation);
    CHECK_FALSE(cleared.state.focused_item);
}

TEST_CASE(PREFIX "publishes hidden-file visibility changes without advancing listing generation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    const auto initial = store.Snapshot();
    CHECK_FALSE(initial.state.shows_hidden_files);

    source.shows_hidden_files = true;
    store.ScheduleRebuild();

    const auto visible = store.Snapshot();
    CHECK(visible.revision == initial.revision + 1);
    CHECK(visible.listing_generation == initial.listing_generation);
    CHECK(visible.state.shows_hidden_files);

    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == visible.revision);
    CHECK(store.Snapshot().listing_generation == visible.listing_generation);
}

TEST_CASE(PREFIX "publishes selection-only changes without advancing listing generation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    source.listing = FocusListing();
    source.item_count = 1;
    source.selected_count = 0;
    source.selected_bytes = 0;
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    const auto initial = store.Snapshot();
    CHECK(initial.state.selected_items.empty());

    source.selected_items = nc::core::PaneSelectedItems{{source.listing->Item(0)}};
    source.selected_count = 1;
    source.selected_bytes = 42;
    store.ScheduleRebuild();

    const auto selected = store.Snapshot();
    CHECK(selected.revision == initial.revision + 1);
    CHECK(selected.listing_generation == initial.listing_generation);
    CHECK(selected.state.selected_count == 1);
    CHECK(selected.state.selected_items == source.selected_items);

    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == selected.revision);
    CHECK(store.Snapshot().listing_generation == selected.listing_generation);

    source.selected_items = {};
    source.selected_count = 0;
    source.selected_bytes = 0;
    store.ScheduleRebuild();

    const auto cleared = store.Snapshot();
    CHECK(cleared.revision == selected.revision + 1);
    CHECK(cleared.listing_generation == selected.listing_generation);
    CHECK(cleared.state.selected_items.empty());
}

TEST_CASE(PREFIX "preserves semantic sort updates through an active lifecycle")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    const nc::core::PaneRequestId request{80};
    const auto descriptor = Navigation();
    REQUIRE(store.ApplyLifecycleEvent(
                Event(1, request, descriptor, nc::core::PaneLifecycleStarted{}))
                .state_changed);
    const auto loading = store.Snapshot();
    CHECK(loading.state.load_phase == PaneLoadPhase::Loading);

    source.sort_state.key = nc::core::PaneSortKey::Size;
    source.sort_state.direction = nc::core::PaneSortDirection::Descending;
    source.sort_state.collation = nc::core::PaneTextCollation::Natural;
    source.sort_state.separates_directories = true;
    source.grouping_state.enabled = true;
    source.grouping_state.key = nc::core::PaneGroupingKey::Size;
    source.view_state.mode = nc::core::PaneViewMode::Gallery;
    source.view_state.layout_index = 2;
    source.history_availability.can_go_back = true;
    source.history_availability.can_go_forward = true;
    source.current_history_entry_id = 55;
    store.ScheduleRebuild();

    const auto sorted = store.Snapshot();
    CHECK(sorted.revision == loading.revision + 1);
    CHECK(sorted.listing_generation == loading.listing_generation);
    CHECK(sorted.state.load_phase == PaneLoadPhase::Loading);
    CHECK(sorted.state.sort_state == source.sort_state);
    CHECK(sorted.state.grouping_state == source.grouping_state);
    CHECK(sorted.state.view_state == source.view_state);
    CHECK(sorted.state.history_availability == source.history_availability);
    CHECK(sorted.state.current_history_entry_id == source.current_history_entry_id);

    REQUIRE(store.ApplyLifecycleEvent(Event(
                2,
                request,
                descriptor,
                nc::core::PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}))
                .state_changed);
    const auto cancelled = store.Snapshot();
    CHECK(cancelled.listing_generation == sorted.listing_generation);
    CHECK(cancelled.state.load_phase == PaneLoadPhase::Loaded);
    CHECK(cancelled.state.sort_state == source.sort_state);
    CHECK(cancelled.state.grouping_state == source.grouping_state);
    CHECK(cancelled.state.view_state == source.view_state);
    CHECK(cancelled.state.history_availability == source.history_availability);
    CHECK(cancelled.state.current_history_entry_id == source.current_history_entry_id);
}

TEST_CASE(PREFIX "preserves hidden-file visibility updates through active lifecycle terminals")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    SECTION("navigation cancellation")
    {
        const nc::core::PaneRequestId request{81};
        const auto descriptor = Navigation();
        REQUIRE(store.ApplyLifecycleEvent(
                    Event(1, request, descriptor, nc::core::PaneLifecycleStarted{}))
                    .state_changed);
        const auto loading = store.Snapshot();
        CHECK(loading.revision == 1);
        CHECK(loading.listing_generation == 0);
        CHECK(loading.state.load_phase == PaneLoadPhase::Loading);

        source.shows_hidden_files = true;
        store.ScheduleRebuild();
        const auto updated = store.Snapshot();
        CHECK(updated.revision == loading.revision + 1);
        CHECK(updated.listing_generation == loading.listing_generation);
        CHECK(updated.state.load_phase == PaneLoadPhase::Loading);
        CHECK(updated.state.shows_hidden_files);

        REQUIRE(store.ApplyLifecycleEvent(Event(
                    2,
                    request,
                    descriptor,
                    nc::core::PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}))
                    .state_changed);
        const auto cancelled = store.Snapshot();
        CHECK(cancelled.revision == updated.revision + 1);
        CHECK(cancelled.listing_generation == updated.listing_generation);
        CHECK(cancelled.state.load_phase == PaneLoadPhase::Loaded);
        CHECK(cancelled.state.shows_hidden_files);
    }

    SECTION("refresh failure")
    {
        const nc::core::PaneRequestId request{82};
        const auto descriptor = Refresh();
        REQUIRE(store.ApplyLifecycleEvent(
                    Event(1, request, descriptor, nc::core::PaneLifecycleStarted{}))
                    .state_changed);
        const auto refreshing = store.Snapshot();
        CHECK(refreshing.revision == 1);
        CHECK(refreshing.listing_generation == 0);
        CHECK(refreshing.state.load_phase == PaneLoadPhase::Refreshing);

        source.shows_hidden_files = true;
        store.ScheduleRebuild();
        const auto updated = store.Snapshot();
        CHECK(updated.revision == refreshing.revision + 1);
        CHECK(updated.listing_generation == refreshing.listing_generation);
        CHECK(updated.state.load_phase == PaneLoadPhase::Refreshing);
        CHECK(updated.state.shows_hidden_files);

        REQUIRE(store.ApplyLifecycleEvent(
                    Event(2, request, descriptor, nc::core::PaneLifecycleFailed{Failure()}))
                    .state_changed);
        const auto failed = store.Snapshot();
        CHECK(failed.revision == updated.revision + 1);
        CHECK(failed.listing_generation == updated.listing_generation);
        CHECK(failed.state.load_phase == PaneLoadPhase::Loaded);
        CHECK(failed.state.shows_hidden_files);
        CHECK(failed.state.visible_error.has_value());
    }
}

TEST_CASE(PREFIX "suppresses duplicate focused-item rebuilds")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    source.listing = FocusListing();
    source.item_count = 1;
    source.focused_item = source.listing->Item(0);
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    int observations = 0;
    const auto ticket = store.Observe([&](const PaneSnapshot &) { ++observations; });
    REQUIRE(ticket);

    store.ScheduleRebuild();

    CHECK(store.Snapshot().revision == 0);
    CHECK(store.Snapshot().listing_generation == 0);
    CHECK(store.Snapshot().state.focused_item == source.focused_item);
    CHECK(observations == 0);
}

TEST_CASE(PREFIX "rejects foreign and out-of-range focused items")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    source.listing = FocusListing();
    source.item_count = 1;
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    int observations = 0;
    const auto ticket = store.Observe([&](const PaneSnapshot &) { ++observations; });
    REQUIRE(ticket);

    const auto foreign_listing = FocusListing();
    source.focused_item = foreign_listing->Item(0);
    store.ScheduleRebuild();

    CHECK(store.Snapshot().revision == 0);
    CHECK(store.Snapshot().listing_generation == 0);
    CHECK_FALSE(store.Snapshot().state.focused_item);
    CHECK(observations == 0);

    source.focused_item = VFSListingItem{source.listing, source.listing->Count()};
    store.ScheduleRebuild();

    CHECK(store.Snapshot().revision == 0);
    CHECK(store.Snapshot().listing_generation == 0);
    CHECK_FALSE(store.Snapshot().state.focused_item);
    CHECK(observations == 0);
}

TEST_CASE(PREFIX "accepts a same-generation refresh commit and advances only listing identity")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(
        PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });
    const auto initial = store.Snapshot();
    const nc::core::PaneRequestId request{71};
    const auto descriptor = Refresh();

    CHECK(store.ApplyLifecycleEvent(
              Event(1, request, descriptor, nc::core::PaneLifecycleStarted{})) ==
          nc::core::PaneLifecycleReducerResult{
              nc::core::PaneLifecycleReducerStatus::Applied, true});
    const auto refreshing = store.Snapshot();
    CHECK(refreshing.revision == initial.revision + 1);
    CHECK(refreshing.listing_generation == initial.listing_generation);
    CHECK(refreshing.state.load_phase == PaneLoadPhase::Refreshing);
    CHECK(refreshing.state.location_generation == initial.state.location_generation);
    CHECK(refreshing.state.listing == initial.state.listing);

    source.listing = DistinctEmptyListing();
    CHECK(store.ApplyLifecycleEvent(Event(
              2,
              request,
              descriptor,
              nc::core::PaneLifecycleCommitted{
                  .controller_generation = source.location_generation,
                  .listing = source.listing,
              })) ==
          nc::core::PaneLifecycleReducerResult{
              nc::core::PaneLifecycleReducerStatus::Applied, true});
    const auto committed = store.Snapshot();
    CHECK(committed.revision == initial.revision + 2);
    CHECK(committed.listing_generation == initial.listing_generation + 1);
    CHECK(committed.state == source);
    CHECK(committed.state.load_phase == PaneLoadPhase::Loaded);
    CHECK(committed.state.location_generation == initial.state.location_generation);
    CHECK_FALSE(committed.state.visible_error);
}

TEST_CASE(PREFIX "coalesces rebuilds and reads the latest source state on the main queue")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source;
    int reader_calls = 0;
    std::vector<std::function<void()>> scheduled_work;
    PaneStoreAdapter store(PaneId{2}, [&] {
        ++reader_calls;
        CHECK(nc::dispatch_is_main_queue());
        return source;
    }, [&](std::function<void()> _work) {
        scheduled_work.emplace_back(std::move(_work));
    });

    int observations = 0;
    auto ticket = store.Observe([&](const PaneSnapshot &) { ++observations; });
    REQUIRE(ticket);

    source.load_phase = PaneLoadPhase::Loading;
    store.ScheduleRebuild();
    source = LoadedState();
    store.ScheduleRebuild();

    REQUIRE(scheduled_work.size() == 1);
    auto rebuild = std::move(scheduled_work.front());
    scheduled_work.clear();
    rebuild();
    REQUIRE(reader_calls == 2);
    CHECK(observations == 1);
    CHECK(store.Snapshot().revision == 1);
    CHECK(store.Snapshot().listing_generation == 1);
    CHECK(store.Snapshot().state == source);

    store.ScheduleRebuild();
    REQUIRE(scheduled_work.size() == 1);
    rebuild = std::move(scheduled_work.front());
    scheduled_work.clear();
    rebuild();
    REQUIRE(reader_calls == 3);
    CHECK(observations == 1);
    CHECK(store.Snapshot().revision == 1);
}

TEST_CASE(PREFIX "recovers after scheduler submission throws")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source;
    int scheduling_attempts = 0;
    PaneStoreAdapter store(PaneId{6}, [&] { return source; }, [&](std::function<void()> _work) {
        ++scheduling_attempts;
        if( scheduling_attempts == 1 )
            throw std::runtime_error("scheduler submission failed");
        _work();
    });

    source = LoadedState();
    CHECK_THROWS_AS(store.ScheduleRebuild(), std::runtime_error);
    CHECK(store.Snapshot().revision == 0);
    CHECK_NOTHROW(store.ScheduleRebuild());
    CHECK(scheduling_attempts == 2);
    CHECK(store.Snapshot().revision == 1);
    CHECK(store.Snapshot().state == source);
}

TEST_CASE(PREFIX "seeds an active request without synthesizing a sequence")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });
    const nc::core::PaneRequestId request{51};
    const auto descriptor = Navigation("/seeded/");

    CHECK(store.SeedActiveLifecycle(nc::core::PaneActiveRequest{request, descriptor}) ==
          nc::core::PaneLifecycleReducerResult{nc::core::PaneLifecycleReducerStatus::Applied, true});
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loading);
    CHECK(store.ApplyLifecycleEvent(Event(
              90,
              request,
              descriptor,
              nc::core::PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}))
              .state_changed);
    CHECK(store.Snapshot().state == source);
}

TEST_CASE(PREFIX "seeds a retained failure and resumes after its observation checkpoint")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });
    const nc::core::PaneRequestId failed_request{61};
    const auto descriptor = Navigation("/retained/" );
    const auto error = Failure();
    const auto failure = Event(
        7, failed_request, descriptor, nc::core::PaneLifecycleFailed{error});

    CHECK(store.SeedRetainedLifecycleFailure(
              nc::core::PaneActiveRequest{failed_request, descriptor}, failure, 9) ==
          nc::core::PaneLifecycleReducerResult{nc::core::PaneLifecycleReducerStatus::Applied, true});
    CHECK(failure.event_sequence == 7);
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Failed);
    REQUIRE(store.Snapshot().state.visible_error);
    CHECK(*store.Snapshot().state.visible_error == error);

    CHECK(store.ApplyLifecycleEvent(Event(
              10,
              nc::core::PaneRequestId{62},
              Navigation("/next/"),
              nc::core::PaneLifecycleStarted{})) ==
          nc::core::PaneLifecycleReducerResult{nc::core::PaneLifecycleReducerStatus::Applied, true});
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loading);
}

TEST_CASE(PREFIX "consumes invisible supersession events before the replacement terminal")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(PaneId{1}, [&] { return source; }, [](std::function<void()> _work) { _work(); });
    int observations = 0;
    const auto ticket = store.Observe([&](const PaneSnapshot &) { ++observations; });

    const nc::core::PaneRequestId first{1};
    const nc::core::PaneRequestId second{2};
    const auto first_descriptor = Navigation("/first/");
    const auto second_descriptor = Navigation("/second/");
    REQUIRE(store.ApplyLifecycleEvent(
                Event(10, first, first_descriptor, nc::core::PaneLifecycleStarted{}))
                .state_changed);
    const auto loading_revision = store.Snapshot().revision;

    const auto superseded = store.ApplyLifecycleEvent(Event(
        11,
        first,
        first_descriptor,
        nc::core::PaneLifecycleSuperseded{.replacement = second}));
    CHECK(superseded.status == nc::core::PaneLifecycleReducerStatus::NoVisibleChange);
    CHECK_FALSE(superseded.state_changed);
    const auto replacement_started = store.ApplyLifecycleEvent(
        Event(12, second, second_descriptor, nc::core::PaneLifecycleStarted{}));
    CHECK(replacement_started.status == nc::core::PaneLifecycleReducerStatus::NoVisibleChange);
    CHECK_FALSE(replacement_started.state_changed);
    CHECK(store.Snapshot().revision == loading_revision);
    CHECK(observations == 1);

    source = LoadedState();
    source.location_generation = 8;
    source.path = "/second/";
    source.display_title = source.path;
    source.listing = DistinctEmptyListing();
    CHECK(store.ApplyLifecycleEvent(Event(
              13,
              second,
              second_descriptor,
              nc::core::PaneLifecycleCommitted{
                  .controller_generation = source.location_generation,
                  .listing = source.listing,
              }))
              .state_changed);
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loaded);
    CHECK(store.Snapshot().state.path == "/second/");
    CHECK(observations == 2);
}

TEST_CASE(PREFIX "rejects stale location generations")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source = LoadedState();
    PaneStoreAdapter store(PaneId{4}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    int observations = 0;
    auto ticket = store.Observe([&](const PaneSnapshot &) { ++observations; });
    REQUIRE(ticket);

    source.location_generation = 6;
    source.path = "/stale/";
    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 0);
    CHECK(store.Snapshot().listing_generation == 0);
    CHECK(store.Snapshot().state.path == "/tmp/");
    CHECK(observations == 0);

    source.location_generation = 8;
    source.path = "/current/";
    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 1);
    CHECK(store.Snapshot().listing_generation == 0);
    CHECK(store.Snapshot().state.path == "/current/");
    CHECK(observations == 1);

    source.listing = DistinctEmptyListing();
    store.ScheduleRebuild();
    CHECK(store.Snapshot().revision == 2);
    CHECK(store.Snapshot().listing_generation == 1);
    CHECK(observations == 2);
}

TEST_CASE(PREFIX "keeps the observed commit stable across a reentrant synchronous rebuild")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source;
    PaneStoreAdapter store(PaneId{5}, [&] { return source; }, [](std::function<void()> _work) { _work(); });

    std::vector<std::pair<uint64_t, uint64_t>> first_observer_revisions;
    std::vector<uint64_t> second_observer_revisions;
    auto first_ticket = store.Observe([&](const PaneSnapshot &_snapshot) {
        const uint64_t revision_before_rebuild = _snapshot.revision;
        if( _snapshot.revision == 1 ) {
            source = LoadedState();
            store.ScheduleRebuild();
        }
        first_observer_revisions.emplace_back(revision_before_rebuild, _snapshot.revision);
    });
    auto second_ticket = store.Observe(
        [&](const PaneSnapshot &_snapshot) { second_observer_revisions.emplace_back(_snapshot.revision); });
    REQUIRE(first_ticket);
    REQUIRE(second_ticket);

    CHECK(store.ApplyLifecycleEvent(
              nc::core::PaneLifecycleEvent{
                  .pane_id = PaneId{5},
                  .request_id = nc::core::PaneRequestId{1},
                  .event_sequence = 1,
                  .descriptor = Navigation(),
                  .payload = nc::core::PaneLifecycleStarted{},
              })
              .state_changed);

    REQUIRE(first_observer_revisions.size() == 2);
    CHECK(first_observer_revisions[0] == std::pair<uint64_t, uint64_t>{1, 1});
    CHECK(first_observer_revisions[1] == std::pair<uint64_t, uint64_t>{2, 2});
    CHECK(second_observer_revisions == std::vector<uint64_t>{1, 2});
    CHECK(store.Snapshot().revision == 2);
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loading);
}

TEST_CASE(PREFIX "delivers a background-scheduled change to observers on the main queue")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneState source;
    PaneStoreAdapter store(PaneId{3}, [&] {
        CHECK(nc::dispatch_is_main_queue());
        return source;
    });

    int observations = 0;
    bool observer_was_on_main = false;
    auto ticket = store.Observe([&](const PaneSnapshot &) {
        ++observations;
        observer_was_on_main = nc::dispatch_is_main_queue();
    });
    REQUIRE(ticket);

    source = LoadedState();
    std::promise<void> scheduled;
    std::future<void> scheduling_finished = scheduled.get_future();
    dispatch_to_background([&store, &scheduled] {
        store.ScheduleRebuild();
        scheduled.set_value();
    });
    REQUIRE(scheduling_finished.wait_for(1s) == std::future_status::ready);

    REQUIRE(RunMainLoopUntil([&] { return observations == 1; }));
    CHECK(observer_was_on_main);
    CHECK(store.Snapshot().state.load_phase == PaneLoadPhase::Loaded);
}
