// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Pane/PaneLifecycleProducer.h>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace nc::core;

PaneRequestDescriptor Navigation(std::string _path)
{
    return PaneRequestDescriptor{
        .kind = PaneRequestKind::Navigation,
        .target = PaneRequestLocation{.path = std::move(_path)},
        .initiated_by_user = true,
    };
}

FileManagerError PermissionError()
{
    return FileManagerError{
        .code = {.domain = std::string{nc::Error::POSIX}, .value = EACCES},
        .category = FileManagerErrorCategory::PermissionError,
        .severity = FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.permission",
        .user_message = "Permission denied.",
        .technical_message = "POSIX permission failure.",
        .original_error = nc::Error{nc::Error::POSIX, EACCES},
    };
}

} // namespace

#define PREFIX "nc::core::PaneLifecycleProducer "

TEST_CASE(PREFIX "classifies accepted-request terminal outcomes")
{
    CHECK_FALSE(IsPaneLifecycleAcceptedTerminal(PaneLifecycleStarted{}));
    CHECK(IsPaneLifecycleAcceptedTerminal(PaneLifecycleCommitted{}));
    CHECK(IsPaneLifecycleAcceptedTerminal(PaneLifecycleFailed{PermissionError()}));
    CHECK(IsPaneLifecycleAcceptedTerminal(PaneLifecycleCancelled{}));
    CHECK(IsPaneLifecycleAcceptedTerminal(PaneLifecycleSuperseded{PaneRequestId{2}}));
    CHECK_FALSE(IsPaneLifecycleAcceptedTerminal(PaneLifecycleRejected{}));
}

TEST_CASE(PREFIX "publishes stable started and committed events")
{
    PaneLifecycleProducer producer(PaneId{42});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const auto descriptor = Navigation("/first/");
    const PaneRequestId request_id = producer.Start(descriptor);
    REQUIRE(producer.Active());
    CHECK(producer.Active()->request_id == request_id);
    CHECK(producer.Active()->descriptor == descriptor);
    CHECK(producer.Finish(request_id, PaneLifecycleCommitted{.controller_generation = 7}) ==
          PaneLifecycleProducer::FinishResult::Published);

    REQUIRE(events.size() == 2);
    CHECK(events[0].pane_id == PaneId{42});
    CHECK(events[0].request_id == request_id);
    CHECK(events[0].event_sequence == 1);
    CHECK(events[0].descriptor == descriptor);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(events[1].request_id == request_id);
    CHECK(events[1].event_sequence == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleCommitted>(events[1].payload));
    CHECK(std::get<PaneLifecycleCommitted>(events[1].payload).controller_generation == 7);
    CHECK_FALSE(producer.Active());
}

TEST_CASE(PREFIX "keeps active state and sequence unchanged when a transactional mutation throws")
{
    PaneLifecycleProducer producer(PaneId{43});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId request_id = producer.Start(Navigation("/transactional-failure/"));
    int mutation_count = 0;
    CHECK_THROWS_AS(
        producer.Finish(
            request_id,
            PaneLifecycleCommitted{.controller_generation = 8},
            [&] {
                ++mutation_count;
                throw std::runtime_error("model mutation failed");
            }),
        std::runtime_error);

    CHECK(mutation_count == 1);
    REQUIRE(producer.Active());
    CHECK(producer.Active()->request_id == request_id);
    REQUIRE(events.size() == 1);
    CHECK(events.front().event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));

    CHECK(producer.Finish(request_id, PaneLifecycleFailed{PermissionError()}) ==
          PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(events.size() == 2);
    CHECK(events.back().event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleFailed>(events.back().payload));
    CHECK_FALSE(producer.Active());
}

TEST_CASE(PREFIX "rejects every lifecycle reentry during a transactional mutation")
{
    PaneLifecycleProducer producer(PaneId{45});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    const PaneRequestId request_id = producer.Start(Navigation("/reentry/"));

    int caught_violations = 0;
    CHECK_THROWS_AS(
        producer.Finish(
            request_id,
            PaneLifecycleCommitted{},
            [&] {
                const auto expect_violation = [&](auto &&_callback) {
                    try {
                        _callback();
                    } catch( const std::logic_error & ) {
                        ++caught_violations;
                    }
                };
                expect_violation(
                    [&] { [[maybe_unused]] const auto id = producer.Start(Navigation("/start/")); });
                expect_violation([&] {
                    [[maybe_unused]] const auto id = producer.Reject(
                        Navigation("/reject/"), PaneLifecycleRejected{});
                });
                expect_violation([&] {
                    [[maybe_unused]] const auto id =
                        producer.SupersedeAndStart(Navigation("/supersede/"));
                });
                expect_violation([&] {
                    [[maybe_unused]] const auto result =
                        producer.Finish(request_id, PaneLifecycleCancelled{});
                });
                expect_violation([&] { producer.Shutdown(); });
            }),
        std::logic_error);

    CHECK(caught_violations == 5);
    REQUIRE(producer.Active());
    CHECK(producer.Active()->request_id == request_id);
    REQUIRE(events.size() == 1);
    CHECK(events.front().event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));

    CHECK_THROWS_AS(
        producer.Finish(request_id, PaneLifecycleCommitted{}, [&] { producer.Shutdown(); }),
        std::logic_error);
    REQUIRE(producer.Active());
    CHECK(producer.Active()->request_id == request_id);
    CHECK(events.size() == 1);

    CHECK(producer.Finish(request_id, PaneLifecycleFailed{PermissionError()}) ==
          PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(events.size() == 2);
    CHECK(events.back().event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleFailed>(events.back().payload));
}

TEST_CASE(PREFIX "defers facade destruction until a transactional mutation unwinds")
{
    PaneLifecycleProducer::ObservationTicket observation;
    std::vector<PaneLifecycleEvent> events;
    auto producer = std::make_unique<PaneLifecycleProducer>(PaneId{46});
    observation = producer->Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    PaneLifecycleProducer *const facade = producer.get();
    const PaneRequestId request_id = facade->Start(Navigation("/destroy-during-mutation/"));

    CHECK_THROWS_AS(
        facade->Finish(request_id, PaneLifecycleCommitted{}, [&] { producer.reset(); }),
        std::logic_error);

    CHECK_FALSE(producer);
    REQUIRE(events.size() == 2);
    CHECK(events[0].request_id == request_id);
    CHECK(events[0].event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(events[1].request_id == request_id);
    CHECK(events[1].event_sequence == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    observation = {};
}

TEST_CASE(PREFIX "publishes a prebuilt commit only after its mutation")
{
    PaneLifecycleProducer producer(PaneId{44});
    std::vector<std::string> order;
    bool model_mutated = false;
    const auto first_observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ) {
            CHECK(model_mutated);
            order.emplace_back("first.committed");
        }
    });
    const auto second_observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ) {
            CHECK(model_mutated);
            order.emplace_back("second.committed");
        }
    });

    const PaneRequestId request_id = producer.Start(Navigation("/transactional-commit/"));
    CHECK(producer.Finish(
              request_id,
              PaneLifecycleCommitted{.controller_generation = 9},
              [&] {
                  model_mutated = true;
                  order.emplace_back("mutation");
              }) == PaneLifecycleProducer::FinishResult::Published);

    CHECK(order == std::vector<std::string>{"mutation", "first.committed", "second.committed"});
    CHECK_FALSE(producer.Active());
}

TEST_CASE(PREFIX "rejects a busy attempt without disturbing the active request")
{
    PaneLifecycleProducer producer(PaneId{1});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId active_id = producer.Start(Navigation("/active/"));
    const PaneRequestId rejected_id = producer.Reject(
        Navigation("/busy/"),
        PaneLifecycleRejected{.reason = PaneRejectionReason::Busy, .conflicting_request = active_id});

    CHECK(rejected_id != active_id);
    REQUIRE(producer.Active());
    CHECK(producer.Active()->request_id == active_id);
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events[1].payload));
    const auto &rejected = std::get<PaneLifecycleRejected>(events[1].payload);
    CHECK(rejected.reason == PaneRejectionReason::Busy);
    CHECK(rejected.conflicting_request == active_id);
}

TEST_CASE(PREFIX "preserves multi-observer FIFO during reentrant replacement completion")
{
    PaneLifecycleProducer producer(PaneId{2});
    std::vector<PaneLifecycleEvent> first_observer_events;
    std::vector<PaneLifecycleEvent> second_observer_events;
    std::optional<PaneRequestId> replacement_id;
    const auto first_observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        first_observer_events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleSuperseded>(_event.payload) ) {
            replacement_id = std::get<PaneLifecycleSuperseded>(_event.payload).replacement;
            CHECK(producer.Finish(*replacement_id, PaneLifecycleCancelled{PaneCancellationReason::User}) ==
                  PaneLifecycleProducer::FinishResult::Published);
        }
    });
    const auto second_observation = producer.Observe(
        [&](const PaneLifecycleEvent &_event) { second_observer_events.emplace_back(_event); });

    const PaneRequestId first_id = producer.Start(Navigation("/first/"));
    replacement_id = producer.SupersedeAndStart(Navigation("/second/"));

    REQUIRE(first_observer_events.size() == 4);
    REQUIRE(second_observer_events.size() == first_observer_events.size());
    CHECK(first_observer_events[0].request_id == first_id);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(first_observer_events[0].payload));
    CHECK(first_observer_events[1].request_id == first_id);
    REQUIRE(std::holds_alternative<PaneLifecycleSuperseded>(first_observer_events[1].payload));
    CHECK(std::get<PaneLifecycleSuperseded>(first_observer_events[1].payload).replacement == *replacement_id);
    CHECK(first_observer_events[2].request_id == *replacement_id);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(first_observer_events[2].payload));
    CHECK(first_observer_events[3].request_id == *replacement_id);
    CHECK(std::holds_alternative<PaneLifecycleCancelled>(first_observer_events[3].payload));
    for( std::size_t index = 0; index < first_observer_events.size(); ++index ) {
        CHECK(first_observer_events[index].event_sequence == index + 1);
        CHECK(second_observer_events[index].event_sequence == first_observer_events[index].event_sequence);
        CHECK(second_observer_events[index].request_id == first_observer_events[index].request_id);
        CHECK(second_observer_events[index].payload.index() == first_observer_events[index].payload.index());
    }
}

TEST_CASE(PREFIX "allocates monotonic request ids and event sequences across every admission path")
{
    PaneLifecycleProducer producer(PaneId{6});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId rejected_id = producer.Reject(
        Navigation("/invalid/"), PaneLifecycleRejected{.reason = PaneRejectionReason::InvalidRequest});
    const PaneRequestId first_id = producer.Start(Navigation("/first/"));
    const PaneRequestId replacement_id = producer.SupersedeAndStart(Navigation("/replacement/"));
    CHECK(producer.Finish(replacement_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::Published);

    CHECK(rejected_id == PaneRequestId{1});
    CHECK(first_id == PaneRequestId{2});
    CHECK(replacement_id == PaneRequestId{3});
    REQUIRE(events.size() == 5);
    CHECK(std::holds_alternative<PaneLifecycleRejected>(events[0].payload));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[1].payload));
    CHECK(std::holds_alternative<PaneLifecycleSuperseded>(events[2].payload));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[3].payload));
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[4].payload));
    for( std::size_t index = 0; index < events.size(); ++index )
        CHECK(events[index].event_sequence == index + 1);
}

TEST_CASE(PREFIX "publishes typed failures and suppresses duplicate and stale terminals")
{
    PaneLifecycleProducer producer(PaneId{3});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId failed_id = producer.Start(Navigation("/denied/"));
    CHECK(producer.Finish(failed_id, PaneLifecycleFailed{PermissionError()}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(producer.Finish(failed_id, PaneLifecycleCancelled{}) ==
          PaneLifecycleProducer::FinishResult::NoActiveRequest);

    const PaneRequestId current_id = producer.Start(Navigation("/current/"));
    CHECK(producer.Finish(failed_id, PaneLifecycleCommitted{}) == PaneLifecycleProducer::FinishResult::StaleRequest);
    CHECK(producer.Finish(current_id, PaneLifecycleCancelled{PaneCancellationReason::QueueStopped}) ==
          PaneLifecycleProducer::FinishResult::Published);

    REQUIRE(events.size() == 4);
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
    const auto &error = std::get<PaneLifecycleFailed>(events[1].payload).error;
    CHECK(error.category == FileManagerErrorCategory::PermissionError);
    CHECK(error.code.domain == nc::Error::POSIX);
    CHECK(error.code.value == EACCES);
    CHECK(error.original_error == nc::Error{nc::Error::POSIX, EACCES});
}

TEST_CASE(PREFIX "subscribes with an active seed and live terminal boundary")
{
    PaneLifecycleProducer producer(PaneId{31});
    const auto request_id = producer.Start(Navigation("/active-before-subscribe/"));
    std::vector<PaneLifecycleEvent> live_events;

    auto subscription = producer.Subscribe(
        [&](const PaneLifecycleEvent &_event) { live_events.emplace_back(_event); });

    REQUIRE(subscription.seed_request);
    CHECK(subscription.seed_request->request_id == request_id);
    CHECK(subscription.seed_request->descriptor == Navigation("/active-before-subscribe/"));
    CHECK_FALSE(subscription.retained_failure);
    CHECK(producer.Finish(request_id, PaneLifecycleCancelled{PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(live_events.size() == 1);
    CHECK(live_events[0].request_id == request_id);
    CHECK(live_events[0].event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleCancelled>(live_events[0].payload));
}

TEST_CASE(PREFIX "reentrant subscription during Failed returns its seed and replay")
{
    PaneLifecycleProducer producer(PaneId{32});
    std::optional<PaneLifecycleProducer::Subscription> nested_subscription;
    std::vector<PaneLifecycleEvent> nested_live_events;
    const auto first_observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleFailed>(_event.payload) ) {
            nested_subscription.emplace(producer.Subscribe(
                [&](const PaneLifecycleEvent &_live) { nested_live_events.emplace_back(_live); }));
        }
    });

    const auto failed_id = producer.Start(Navigation("/failed/"));
    const auto failure = PermissionError();
    CHECK(producer.Finish(failed_id, PaneLifecycleFailed{failure}) ==
          PaneLifecycleProducer::FinishResult::Published);

    REQUIRE(nested_subscription);
    REQUIRE(nested_subscription->seed_request);
    CHECK(nested_subscription->seed_request->request_id == failed_id);
    REQUIRE(nested_subscription->retained_failure);
    CHECK(nested_subscription->retained_failure->request_id == failed_id);
    CHECK(nested_subscription->retained_failure->event_sequence == 2);
    CHECK(nested_subscription->checkpoint_sequence == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(
        nested_subscription->retained_failure->payload));
    CHECK(std::get<PaneLifecycleFailed>(nested_subscription->retained_failure->payload).error ==
          failure);
    CHECK(nested_live_events.empty());

    const auto next_id = producer.Start(Navigation("/next/"));
    REQUIRE(nested_live_events.size() == 1);
    CHECK(nested_live_events[0].request_id == next_id);
    CHECK(nested_live_events[0].event_sequence == 3);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(nested_live_events[0].payload));
}

TEST_CASE(PREFIX "subscription during Superseded waits for the queued replacement Started")
{
    PaneLifecycleProducer producer(PaneId{33});
    std::optional<PaneLifecycleProducer::Subscription> nested_subscription;
    std::vector<PaneLifecycleEvent> nested_live_events;
    const auto first_observation = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleSuperseded>(_event.payload) ) {
            nested_subscription.emplace(producer.Subscribe(
                [&](const PaneLifecycleEvent &_live) { nested_live_events.emplace_back(_live); }));
        }
    });

    [[maybe_unused]] const auto first_id = producer.Start(Navigation("/first/"));
    const auto replacement_id = producer.SupersedeAndStart(Navigation("/replacement/"));

    REQUIRE(nested_subscription);
    CHECK_FALSE(nested_subscription->seed_request);
    CHECK_FALSE(nested_subscription->retained_failure);
    REQUIRE(nested_live_events.size() == 1);
    CHECK(nested_live_events[0].request_id == replacement_id);
    CHECK(nested_live_events[0].event_sequence == 3);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(nested_live_events[0].payload));
}

TEST_CASE(PREFIX "subscription during Started seeds a terminal queued by an earlier observer")
{
    PaneLifecycleProducer producer(PaneId{35});
    std::optional<PaneLifecycleProducer::Subscription> nested_subscription;
    std::vector<PaneLifecycleEvent> nested_live_events;
    const auto terminal_first = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
            CHECK(producer.Finish(
                      _event.request_id,
                      PaneLifecycleCancelled{PaneCancellationReason::User}) ==
                  PaneLifecycleProducer::FinishResult::Published);
        }
    });
    const auto subscribe_second = producer.Observe([&](const PaneLifecycleEvent &_event) {
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
            nested_subscription.emplace(producer.Subscribe(
                [&](const PaneLifecycleEvent &_live) { nested_live_events.emplace_back(_live); }));
        }
    });

    const auto request_id = producer.Start(Navigation("/terminal-queued/"));

    REQUIRE(nested_subscription);
    REQUIRE(nested_subscription->seed_request);
    CHECK(nested_subscription->seed_request->request_id == request_id);
    CHECK_FALSE(nested_subscription->retained_failure);
    REQUIRE(nested_live_events.size() == 1);
    CHECK(nested_live_events[0].request_id == request_id);
    CHECK(nested_live_events[0].event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleCancelled>(nested_live_events[0].payload));
}

TEST_CASE(PREFIX "retains failure across rejections and rebases replay to the observation checkpoint")
{
    PaneLifecycleProducer producer(PaneId{34});
    const auto failed_id = producer.Start(Navigation("/failed/"));
    CHECK(producer.Finish(failed_id, PaneLifecycleFailed{PermissionError()}) ==
          PaneLifecycleProducer::FinishResult::Published);
    [[maybe_unused]] const auto rejected_id = producer.Reject(
        Navigation("/rejected/"),
        PaneLifecycleRejected{.reason = PaneRejectionReason::InvalidRequest});

    std::vector<PaneLifecycleEvent> live_events;
    auto subscription = producer.Subscribe(
        [&](const PaneLifecycleEvent &_event) { live_events.emplace_back(_event); });
    REQUIRE(subscription.seed_request);
    CHECK(subscription.seed_request->request_id == failed_id);
    REQUIRE(subscription.retained_failure);
    CHECK(subscription.retained_failure->request_id == failed_id);
    CHECK(subscription.retained_failure->event_sequence == 2);
    CHECK(subscription.checkpoint_sequence == 3);

    const auto next_id = producer.Start(Navigation("/next/"));
    REQUIRE(live_events.size() == 1);
    CHECK(live_events[0].request_id == next_id);
    CHECK(live_events[0].event_sequence == 4);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(live_events[0].payload));

    auto after_started = producer.Subscribe([](const PaneLifecycleEvent &) {});
    REQUIRE(after_started.seed_request);
    CHECK(after_started.seed_request->request_id == next_id);
    CHECK_FALSE(after_started.retained_failure);
    CHECK(producer.Finish(next_id, PaneLifecycleCancelled{PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    auto after_cancel = producer.Subscribe([](const PaneLifecycleEvent &) {});
    CHECK_FALSE(after_cancel.seed_request);
    CHECK_FALSE(after_cancel.retained_failure);
}

TEST_CASE(PREFIX "observation tickets scope delivery and shutdown terminates once")
{
    PaneLifecycleProducer producer(PaneId{4});
    std::vector<PaneLifecycleEvent> first_observer_events;
    std::vector<PaneLifecycleEvent> second_observer_events;
    auto first = producer.Observe(
        [&](const PaneLifecycleEvent &_event) { first_observer_events.emplace_back(_event); });
    {
        const auto second = producer.Observe(
            [&](const PaneLifecycleEvent &_event) { second_observer_events.emplace_back(_event); });
        [[maybe_unused]] const PaneRequestId request_id = producer.Start(Navigation("/active/"));
    }

    producer.Shutdown();
    producer.Shutdown();

    REQUIRE(first_observer_events.size() == 2);
    REQUIRE(second_observer_events.size() == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(first_observer_events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(first_observer_events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(first_observer_events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    CHECK(producer.Finish(first_observer_events[0].request_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::ProducerShutdown);
}

TEST_CASE(PREFIX "enforces admission and shutdown boundaries without allocating hidden events")
{
    PaneLifecycleProducer producer(PaneId{7});
    std::vector<PaneLifecycleEvent> events;
    const auto observation = producer.Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    CHECK_THROWS_AS(producer.SupersedeAndStart(Navigation("/missing/")), std::logic_error);
    CHECK(events.empty());

    const PaneRequestId active_id = producer.Start(Navigation("/active/"));
    CHECK_THROWS_AS(producer.Start(Navigation("/busy/")), std::logic_error);
    REQUIRE(events.size() == 1);
    CHECK(events[0].request_id == active_id);

    producer.Shutdown();
    producer.Shutdown();
    REQUIRE(events.size() == 2);
    CHECK(events[1].request_id == active_id);
    CHECK(events[1].event_sequence == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    CHECK_FALSE(producer.Active());
    CHECK(producer.Finish(active_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::ProducerShutdown);
    CHECK_THROWS_AS(producer.Start(Navigation("/after-shutdown/")), std::logic_error);
    CHECK_THROWS_AS(
        producer.Reject(Navigation("/after-shutdown/"), PaneLifecycleRejected{}), std::logic_error);
    CHECK_THROWS_AS(producer.SupersedeAndStart(Navigation("/after-shutdown/")), std::logic_error);
    CHECK(events.size() == 2);

    PaneLifecycleProducer idle_producer(PaneId{70});
    std::vector<PaneLifecycleEvent> idle_events;
    const auto idle_observation =
        idle_producer.Observe([&](const PaneLifecycleEvent &_event) { idle_events.emplace_back(_event); });
    idle_producer.Shutdown();
    idle_producer.Shutdown();
    CHECK(idle_events.empty());
    CHECK_FALSE(idle_producer.Active());
}

TEST_CASE(PREFIX "keeps Start atomic when counters cannot admit its publication")
{
    CHECK_THROWS_AS(
        detail::PaneLifecycleProducerTestAccess::Make(
            PaneId{71}, detail::PaneLifecycleCounterLimits{.maximum_request_id = 0}),
        std::invalid_argument);
    CHECK_THROWS_AS(
        detail::PaneLifecycleProducerTestAccess::Make(
            PaneId{71}, detail::PaneLifecycleCounterLimits{.maximum_event_sequence = 0}),
        std::invalid_argument);

    auto producer = detail::PaneLifecycleProducerTestAccess::Make(
        PaneId{71},
        detail::PaneLifecycleCounterLimits{.maximum_request_id = 2, .maximum_event_sequence = 1});
    std::vector<PaneLifecycleEvent> events;
    const auto observation =
        producer->Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    CHECK_THROWS_AS(producer->Start(Navigation("/must-not-start/")), std::overflow_error);
    CHECK_FALSE(producer->Active());
    CHECK(events.empty());

    const PaneRequestId rejected_id = producer->Reject(
        Navigation("/rejected/"), PaneLifecycleRejected{.reason = PaneRejectionReason::InvalidRequest});

    CHECK_FALSE(producer->Active());
    REQUIRE(events.size() == 1);
    CHECK(events[0].request_id == rejected_id);
    CHECK(events[0].event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleRejected>(events[0].payload));
    producer->Shutdown();
}

TEST_CASE(PREFIX "prevents an active rejection from consuming reserved terminal capacity")
{
    auto producer = detail::PaneLifecycleProducerTestAccess::Make(
        PaneId{72},
        detail::PaneLifecycleCounterLimits{.maximum_request_id = 2, .maximum_event_sequence = 2});
    std::vector<PaneLifecycleEvent> events;
    const auto observation =
        producer->Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId request_id = producer->Start(Navigation("/active/"));
    CHECK_THROWS_AS(
        producer->Reject(Navigation("/must-not-reject/"), PaneLifecycleRejected{}), std::overflow_error);

    REQUIRE(producer->Active());
    CHECK(producer->Active()->request_id == request_id);
    REQUIRE(events.size() == 1);
    CHECK(events[0].request_id == request_id);
    CHECK(events[0].event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));

    CHECK(producer->Finish(request_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(events.size() == 2);
    CHECK(events[1].request_id == request_id);
    CHECK(events[1].event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[1].payload));
    CHECK_FALSE(producer->Active());
}

TEST_CASE(PREFIX "keeps SupersedeAndStart atomic when its event batch cannot fit")
{
    auto producer = detail::PaneLifecycleProducerTestAccess::Make(
        PaneId{73},
        detail::PaneLifecycleCounterLimits{.maximum_request_id = 2, .maximum_event_sequence = 2});
    std::vector<PaneLifecycleEvent> events;
    const auto observation =
        producer->Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId request_id = producer->Start(Navigation("/active/"));
    CHECK_THROWS_AS(producer->SupersedeAndStart(Navigation("/must-not-start/")), std::overflow_error);

    REQUIRE(producer->Active());
    CHECK(producer->Active()->request_id == request_id);
    CHECK(producer->Active()->descriptor == Navigation("/active/"));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));

    CHECK(producer->Finish(request_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(events.size() == 2);
    CHECK(events[1].request_id == request_id);
    CHECK(events[1].event_sequence == 2);
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[1].payload));
    CHECK_FALSE(producer->Active());
}

TEST_CASE(PREFIX "uses the final reserved sequence for a positive terminal boundary")
{
    auto producer = detail::PaneLifecycleProducerTestAccess::Make(
        PaneId{74},
        detail::PaneLifecycleCounterLimits{.maximum_request_id = 1, .maximum_event_sequence = 2});
    std::vector<PaneLifecycleEvent> events;
    const auto observation =
        producer->Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const PaneRequestId request_id = producer->Start(Navigation("/boundary/"));
    CHECK(producer->Finish(request_id, PaneLifecycleCancelled{PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);

    REQUIRE(events.size() == 2);
    CHECK(events[0].event_sequence == 1);
    CHECK(events[1].event_sequence == 2);
    CHECK(events[0].request_id == request_id);
    CHECK(events[1].request_id == request_id);
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason == PaneCancellationReason::User);
    CHECK_FALSE(producer->Active());
}

TEST_CASE(PREFIX "supports empty observers and ticket reset from inside a callback")
{
    PaneLifecycleProducer producer(PaneId{8});
    auto empty_observation = producer.Observe({});
    CHECK_FALSE(static_cast<bool>(empty_observation));

    PaneLifecycleProducer::ObservationTicket self_removing_observation;
    std::size_t callback_count = 0;
    self_removing_observation = producer.Observe([&](const PaneLifecycleEvent &) {
        ++callback_count;
        self_removing_observation = {};
    });

    const PaneRequestId request_id = producer.Start(Navigation("/once/"));
    CHECK(callback_count == 1);
    CHECK_FALSE(static_cast<bool>(self_removing_observation));
    CHECK(producer.Finish(request_id, PaneLifecycleCommitted{}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(callback_count == 1);
}

TEST_CASE(PREFIX "observation ticket remains safe after producer destruction")
{
    PaneLifecycleProducer::ObservationTicket observation;
    std::vector<PaneLifecycleEvent> events;
    {
        const auto producer = std::make_unique<PaneLifecycleProducer>(PaneId{5});
        observation = producer->Observe([&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
        [[maybe_unused]] const PaneRequestId request_id = producer->Start(Navigation("/active/"));
    }

    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    observation = {};
}

TEST_CASE(PREFIX "survives producer destruction from inside a Started callback")
{
    PaneLifecycleProducer::ObservationTicket observation;
    std::vector<PaneLifecycleEvent> events;
    auto producer = std::make_unique<PaneLifecycleProducer>(PaneId{9});
    observation = producer->Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) )
            producer.reset();
    });

    const PaneRequestId request_id = producer->Start(Navigation("/destroy-from-callback/"));

    CHECK_FALSE(producer);
    REQUIRE(events.size() == 2);
    CHECK(events[0].request_id == request_id);
    CHECK(events[0].event_sequence == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(events[1].request_id == request_id);
    CHECK(events[1].event_sequence == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    observation = {};
}
