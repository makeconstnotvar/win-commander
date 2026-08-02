// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Pane/PanelControllerLifecycle.h>
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

PaneRequestDescriptor Refresh()
{
    return PaneRequestDescriptor{.kind = PaneRequestKind::Refresh};
}

PanelControllerLifecycle::AdmissionProbe Allow()
{
    return [](const PanelControllerLifecycleProbeContext &) { return PanelControllerLifecycleAdmission{}; };
}

FileManagerError MappedException(std::exception_ptr _exception)
{
    std::string message = "Unknown exception.";
    try {
        std::rethrow_exception(std::move(_exception));
    } catch( const std::exception &_error ) {
        message = _error.what();
    } catch( ... ) {
    }

    return FileManagerError{
        .code = {.domain = "PanelControllerLifecycleTests", .value = 7},
        .category = FileManagerErrorCategory::UnknownError,
        .severity = FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.unknown",
        .user_message = "Request failed.",
        .technical_message = std::move(message),
        .original_error = nc::Error{"PanelControllerLifecycleTests", 7},
    };
}

} // namespace

#define PREFIX "nc::core::PanelControllerLifecycle "

TEST_CASE(PREFIX "requires exception mapping and proxies producer identity and observation")
{
    CHECK_THROWS_AS(PanelControllerLifecycle(PaneId{1}, {}), std::invalid_argument);

    PanelControllerLifecycle lifecycle(PaneId{42}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    CHECK(lifecycle.Pane() == PaneId{42});
    CHECK_FALSE(lifecycle.Active());

    bool scheduler_saw_started = false;
    const auto result = lifecycle.SubmitNavigation(
        Refresh(),
        PaneNavigationExecution::Asynchronous,
        Allow(),
        [&](const PaneRequestId _request_id) {
            scheduler_saw_started = events.size() == 1 &&
                                    std::holds_alternative<PaneLifecycleStarted>(events.front().payload);
            REQUIRE(lifecycle.Active());
            CHECK(lifecycle.Active()->request_id == _request_id);
            CHECK(lifecycle.Active()->descriptor.kind == PaneRequestKind::Navigation);
        });

    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Accepted);
    REQUIRE(result.request_id);
    CHECK(scheduler_saw_started);
    CHECK(lifecycle.Cancel(*result.request_id, PaneCancellationReason::User) ==
          PaneLifecycleProducer::FinishResult::Published);
}

TEST_CASE(PREFIX "evaluates invalid unavailable and external-work reasons before active policy")
{
    PanelControllerLifecycle lifecycle(PaneId{2}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    const auto active = lifecycle.SubmitNavigation(
        Navigation("/active/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
    REQUIRE(active.request_id);

    const auto invalid = lifecycle.SubmitRefresh(
        Refresh(),
        [](const PanelControllerLifecycleProbeContext &) {
            return PanelControllerLifecycleAdmission{
                .valid = false,
                .available = false,
                .has_external_loading_work = true,
            };
        },
        [](PaneRequestId) { FAIL("invalid request must not be scheduled"); });
    CHECK(invalid.status == PanelControllerLifecycleSubmissionStatus::Rejected);
    CHECK(invalid.rejection_reason == PaneRejectionReason::InvalidRequest);

    const auto unavailable = lifecycle.SubmitRefresh(
        Refresh(),
        [](const PanelControllerLifecycleProbeContext &) {
            return PanelControllerLifecycleAdmission{.available = false, .has_external_loading_work = true};
        },
        [](PaneRequestId) { FAIL("unavailable request must not be scheduled"); });
    CHECK(unavailable.rejection_reason == PaneRejectionReason::Unavailable);

    const auto external = lifecycle.SubmitRefresh(
        Refresh(),
        [](const PanelControllerLifecycleProbeContext &) {
            return PanelControllerLifecycleAdmission{.has_external_loading_work = true};
        },
        [](PaneRequestId) { FAIL("externally busy request must not be scheduled"); });
    CHECK(external.rejection_reason == PaneRejectionReason::Busy);
    CHECK(lifecycle.Active()->request_id == active.request_id);

    REQUIRE(events.size() == 4);
    for( std::size_t index = 1; index < events.size(); ++index ) {
        REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events[index].payload));
        CHECK_FALSE(std::get<PaneLifecycleRejected>(events[index].payload).admission_error);
    }
    CHECK_FALSE(std::get<PaneLifecycleRejected>(events[3].payload).conflicting_request);
}

TEST_CASE(PREFIX "turns an admission probe exception into an unavailable rejected attempt")
{
    PanelControllerLifecycle lifecycle(PaneId{20}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    const auto active = lifecycle.SubmitNavigation(
        Navigation("/active/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
    REQUIRE(active.request_id);

    const auto result = lifecycle.SubmitRefresh(
        Refresh(),
        [](const PanelControllerLifecycleProbeContext &) -> PanelControllerLifecycleAdmission {
            throw std::runtime_error("probe failed");
        },
        [](PaneRequestId) { FAIL("failed probe must not schedule work"); });

    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Rejected);
    CHECK(result.rejection_reason == PaneRejectionReason::Unavailable);
    REQUIRE(result.rejection_error);
    CHECK(result.rejection_error->code.domain == "PanelControllerLifecycleTests");
    CHECK(result.rejection_error->code.value == 7);
    CHECK(result.rejection_error->technical_message == "probe failed");
    REQUIRE(events.size() == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events[1].payload));
    const auto &rejected = std::get<PaneLifecycleRejected>(events[1].payload);
    CHECK(rejected.reason == PaneRejectionReason::Unavailable);
    CHECK_FALSE(rejected.conflicting_request);
    REQUIRE(rejected.admission_error);
    CHECK(rejected.admission_error->code.domain == "PanelControllerLifecycleTests");
    CHECK(rejected.admission_error->code.value == 7);
    CHECK(rejected.admission_error->technical_message == "probe failed");
    REQUIRE(lifecycle.Active());
    CHECK(lifecycle.Active()->request_id == active.request_id);
}

TEST_CASE(PREFIX "uses a typed fallback when admission exception mapping fails")
{
    PanelControllerLifecycle lifecycle(PaneId{25}, [](std::exception_ptr) -> FileManagerError {
        throw std::runtime_error("mapper failed");
    });
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });

    const auto result = lifecycle.SubmitRefresh(
        Refresh(),
        [](const PanelControllerLifecycleProbeContext &) -> PanelControllerLifecycleAdmission {
            throw std::runtime_error("probe failed");
        },
        [](PaneRequestId) { FAIL("failed probe must not schedule work"); });

    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Rejected);
    REQUIRE(result.rejection_error);
    CHECK(result.rejection_error->code.domain == "PanelControllerLifecycle");
    CHECK(result.rejection_error->code.value == 1);
    CHECK(result.rejection_error->technical_message ==
          "PanelControllerLifecycle exception mapper failed.");
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events[0].payload));
    const auto &rejected = std::get<PaneLifecycleRejected>(events[0].payload);
    REQUIRE(rejected.admission_error);
    CHECK(rejected.admission_error->code.domain == "PanelControllerLifecycle");
    CHECK(rejected.admission_error->code.value == 1);
    CHECK(rejected.admission_error->category == FileManagerErrorCategory::UnknownError);
    CHECK(rejected.admission_error->technical_message ==
          "PanelControllerLifecycle exception mapper failed.");
    CHECK_FALSE(lifecycle.Active());
}

TEST_CASE(PREFIX "rejects asynchronous navigation behind navigation and supersedes refresh")
{
    SECTION("navigation behind navigation is busy")
    {
        PanelControllerLifecycle lifecycle(PaneId{3}, MappedException);
        const auto first = lifecycle.SubmitNavigation(
            Navigation("/first/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);

        const auto second = lifecycle.SubmitNavigation(
            Navigation("/second/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {
                FAIL("busy navigation must not be scheduled");
            });
        CHECK(second.status == PanelControllerLifecycleSubmissionStatus::Rejected);
        CHECK(second.rejection_reason == PaneRejectionReason::Busy);
        CHECK(lifecycle.Active()->request_id == first.request_id);
    }

    SECTION("navigation supersedes refresh")
    {
        PanelControllerLifecycle lifecycle(PaneId{4}, MappedException);
        const auto refresh = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        REQUIRE(refresh.request_id);
        const auto navigation = lifecycle.SubmitNavigation(
            Navigation("/next/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(navigation.request_id);
        CHECK(navigation.status == PanelControllerLifecycleSubmissionStatus::Accepted);
        CHECK(lifecycle.Active()->request_id == navigation.request_id);
        CHECK(lifecycle.Active()->descriptor.kind == PaneRequestKind::Navigation);
    }
}

TEST_CASE(PREFIX "synchronous navigation supersedes active work and is unsupported reentrantly")
{
    SECTION("synchronous navigation supersedes navigation")
    {
        PanelControllerLifecycle lifecycle(PaneId{5}, MappedException);
        const auto first = lifecycle.SubmitNavigation(
            Navigation("/first/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        const auto second = lifecycle.SubmitNavigation(
            Navigation("/second/"), PaneNavigationExecution::Synchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        REQUIRE(second.request_id);
        CHECK(lifecycle.Active()->request_id == second.request_id);
    }

    SECTION("synchronous navigation supersedes refresh")
    {
        PanelControllerLifecycle lifecycle(PaneId{50}, MappedException);
        const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        const auto second = lifecycle.SubmitNavigation(
            Navigation("/second/"), PaneNavigationExecution::Synchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        REQUIRE(second.request_id);
        CHECK(lifecycle.Active()->request_id == second.request_id);
        CHECK(lifecycle.Active()->descriptor.kind == PaneRequestKind::Navigation);
    }

    SECTION("synchronous submission from Started is rejected without an event or exception")
    {
        PanelControllerLifecycle lifecycle(PaneId{6}, MappedException);
        std::vector<PaneLifecycleEvent> events;
        PanelControllerLifecycleSubmissionResult nested;
        bool attempted = false;
        bool outer_scheduler_called = false;
        bool nested_scheduler_called = false;
        const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
            events.emplace_back(_event);
            if( !attempted && std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
                attempted = true;
                nested = lifecycle.SubmitNavigation(
                    Navigation("/nested/"),
                    PaneNavigationExecution::Synchronous,
                    Allow(),
                    [&](PaneRequestId) { nested_scheduler_called = true; });
            }
        });

        const auto outer = lifecycle.SubmitRefresh(
            Refresh(), Allow(), [&](PaneRequestId) { outer_scheduler_called = true; });
        CHECK(nested.status ==
              PanelControllerLifecycleSubmissionStatus::SynchronousReentrancyUnsupported);
        CHECK_FALSE(nested.request_id);
        REQUIRE(outer.request_id);
        CHECK(outer_scheduler_called);
        CHECK_FALSE(nested_scheduler_called);
        REQUIRE(events.size() == 1);
        CHECK(events.front().request_id == outer.request_id);
        REQUIRE(lifecycle.Active());
        CHECK(lifecycle.Active()->request_id == outer.request_id);
    }
}

TEST_CASE(PREFIX "refresh is busy behind navigation and latest wins behind refresh")
{
    SECTION("refresh behind navigation is busy")
    {
        PanelControllerLifecycle lifecycle(PaneId{7}, MappedException);
        const auto navigation = lifecycle.SubmitNavigation(
            Navigation("/active/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        const auto refresh = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {
            FAIL("busy refresh must not be scheduled");
        });
        REQUIRE(navigation.request_id);
        CHECK(refresh.rejection_reason == PaneRejectionReason::Busy);
        CHECK(lifecycle.Active()->request_id == navigation.request_id);
    }

    SECTION("refresh supersedes refresh")
    {
        PanelControllerLifecycle lifecycle(PaneId{8}, MappedException);
        const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        const auto second = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        REQUIRE(second.request_id);
        CHECK(lifecycle.Active()->request_id == second.request_id);
    }
}

TEST_CASE(PREFIX "skips scheduling when a Started callback cancels the accepted identity")
{
    PanelControllerLifecycle lifecycle(PaneId{9}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
            CHECK(lifecycle.Cancel(_event.request_id, PaneCancellationReason::User) ==
                  PaneLifecycleProducer::FinishResult::Published);
        }
    });

    const auto result = lifecycle.SubmitNavigation(
        Navigation("/cancelled/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {
            FAIL("cancelled identity must not reach the scheduler");
        });
    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Accepted);
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK_FALSE(lifecycle.Active());
}

TEST_CASE(PREFIX "maps scheduler and mutation exceptions to one Failed terminal")
{
    SECTION("scheduler exception")
    {
        PanelControllerLifecycle lifecycle(PaneId{10}, MappedException);
        std::vector<PaneLifecycleEvent> events;
        const auto observation = lifecycle.Observe(
            [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
        const auto result = lifecycle.SubmitNavigation(
            Navigation("/scheduler/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {
                throw std::runtime_error("scheduler failed");
            });
        REQUIRE(result.request_id);
        REQUIRE(events.size() == 2);
        REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
        CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.technical_message ==
              "scheduler failed");
        CHECK_FALSE(lifecycle.Active());
    }

    SECTION("mutation exception")
    {
        PanelControllerLifecycle lifecycle(PaneId{11}, MappedException);
        std::vector<PaneLifecycleEvent> events;
        const auto observation = lifecycle.Observe(
            [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
        const auto result = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        REQUIRE(result.request_id);
        CHECK(lifecycle.Commit(*result.request_id, PaneLifecycleCommitted{}, [] {
                  throw std::runtime_error("mutation failed");
              }) == PaneLifecycleProducer::FinishResult::Published);
        REQUIRE(events.size() == 2);
        REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
        CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.technical_message ==
              "mutation failed");
        CHECK_FALSE(lifecycle.Active());
    }
}

TEST_CASE(PREFIX "does not run a stale commit mutation")
{
    PanelControllerLifecycle lifecycle(PaneId{12}, MappedException);
    const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    const auto second = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(first.request_id);
    REQUIRE(second.request_id);
    int mutation_count = 0;
    CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [&] {
              ++mutation_count;
          }) == PaneLifecycleProducer::FinishResult::StaleRequest);
    CHECK(mutation_count == 0);
    CHECK(lifecycle.Active()->request_id == second.request_id);
}

TEST_CASE(PREFIX "treats cancellation during a commit mutation as a failed mutation")
{
    PanelControllerLifecycle lifecycle(PaneId{21}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { events.emplace_back(_event); });
    const auto request = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(request.request_id);

    bool nested_cancel_threw = false;
    CHECK(lifecycle.Commit(*request.request_id, PaneLifecycleCommitted{}, [&] {
              try {
                  [[maybe_unused]] const auto result =
                      lifecycle.Cancel(*request.request_id, PaneCancellationReason::User);
              } catch( const std::logic_error & ) {
                  nested_cancel_threw = true;
              }
          }) == PaneLifecycleProducer::FinishResult::Published);

    CHECK(nested_cancel_threw);
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
    CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.technical_message ==
          "PanelControllerLifecycle commit mutation attempted lifecycle reentry");
    CHECK_FALSE(lifecycle.Active());
}

TEST_CASE(PREFIX "defers admissions until mutation and Committed reach every observer")
{
    PanelControllerLifecycle lifecycle(PaneId{13}, MappedException);
    std::vector<std::string> order;
    std::optional<PaneRequestId> replacement;
    bool track_commit = false;
    const auto first_observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        if( !track_commit )
            return;
        if( std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ) {
            order.emplace_back("first.committed");
            const auto deferred = lifecycle.SubmitRefresh(
                Refresh(),
                Allow(),
                [&](const PaneRequestId _request_id) {
                    replacement = _request_id;
                    order.emplace_back("scheduler");
                });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
        }
        else if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
            order.emplace_back("first.started");
        }
    });
    const auto second_observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        if( !track_commit )
            return;
        if( std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
            order.emplace_back("second.committed");
        else if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) )
            order.emplace_back("second.started");
    });

    const auto first = lifecycle.SubmitNavigation(
        Navigation("/commit/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
    REQUIRE(first.request_id);
    track_commit = true;
    CHECK(lifecycle.Commit(*first.request_id,
                           PaneLifecycleCommitted{.controller_generation = 17},
                           [&] {
              order.emplace_back("mutation");
          }) == PaneLifecycleProducer::FinishResult::Published);

    CHECK(order == std::vector<std::string>{"mutation",
                                            "first.committed",
                                            "second.committed",
                                            "first.started",
                                            "second.started",
                                            "scheduler"});
    REQUIRE(replacement);
    CHECK(lifecycle.Active()->request_id == replacement);
}

TEST_CASE(PREFIX "resolves only submissions that actually entered the deferred queue")
{
    SECTION("immediate results do not notify the deferred observer")
    {
        PanelControllerLifecycle lifecycle(PaneId{26}, MappedException);
        int resolution_count = 0;
        const auto accepted = lifecycle.SubmitNavigation(
            Navigation("/active/"),
            PaneNavigationExecution::Asynchronous,
            Allow(),
            [](PaneRequestId) {},
            [&](const PanelControllerLifecycleSubmissionResult &) { ++resolution_count; });
        REQUIRE(accepted.request_id);
        CHECK(resolution_count == 0);

        const auto rejected = lifecycle.SubmitNavigation(
            Navigation("/busy/"),
            PaneNavigationExecution::Asynchronous,
            Allow(),
            [](PaneRequestId) { FAIL("busy submission must not be scheduled"); },
            [&](const PanelControllerLifecycleSubmissionResult &) { ++resolution_count; });
        CHECK(rejected.status == PanelControllerLifecycleSubmissionStatus::Rejected);
        CHECK(resolution_count == 0);
    }

    SECTION("accepted deferred submission notifies exactly once after scheduling")
    {
        PanelControllerLifecycle lifecycle(PaneId{27}, MappedException);
        int resolution_count = 0;
        bool scheduled = false;
        bool queued = false;
        const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
            if( queued || !std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
                return;
            queued = true;
            const auto deferred = lifecycle.SubmitRefresh(
                Refresh(),
                Allow(),
                [&](PaneRequestId) { scheduled = true; },
                [&](const PanelControllerLifecycleSubmissionResult &_resolution) {
                    ++resolution_count;
                    CHECK(scheduled);
                    CHECK(_resolution.status == PanelControllerLifecycleSubmissionStatus::Accepted);
                    REQUIRE(_resolution.request_id);
                });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
            CHECK(resolution_count == 0);
        });

        const auto first = lifecycle.SubmitNavigation(
            Navigation("/first/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [] {}) ==
              PaneLifecycleProducer::FinishResult::Published);
        CHECK(scheduled);
        CHECK(resolution_count == 1);
    }
}

TEST_CASE(PREFIX "reports deferred policy and probe rejections exactly once")
{
    SECTION("busy active-policy rejection")
    {
        PanelControllerLifecycle lifecycle(PaneId{28}, MappedException);
        int resolution_count = 0;
        bool queued = false;
        const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
            if( queued || !std::holds_alternative<PaneLifecycleStarted>(_event.payload) )
                return;
            queued = true;
            const auto deferred = lifecycle.SubmitNavigation(
                Navigation("/busy/"),
                PaneNavigationExecution::Asynchronous,
                Allow(),
                [](PaneRequestId) { FAIL("busy deferred navigation must not be scheduled"); },
                [&](const PanelControllerLifecycleSubmissionResult &_resolution) {
                    ++resolution_count;
                    CHECK(_resolution.status == PanelControllerLifecycleSubmissionStatus::Rejected);
                    CHECK(_resolution.rejection_reason == PaneRejectionReason::Busy);
                    CHECK_FALSE(_resolution.rejection_error);
                });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
        });

        const auto active = lifecycle.SubmitNavigation(
            Navigation("/active/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
        REQUIRE(active.request_id);
        CHECK(resolution_count == 1);
        REQUIRE(lifecycle.Active());
        CHECK(lifecycle.Active()->request_id == active.request_id);
    }

    SECTION("unavailable admission rejection")
    {
        PanelControllerLifecycle lifecycle(PaneId{29}, MappedException);
        int resolution_count = 0;
        bool queued = false;
        const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
            if( queued || !std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
                return;
            queued = true;
            const auto deferred = lifecycle.SubmitRefresh(
                Refresh(),
                [](const PanelControllerLifecycleProbeContext &) {
                    return PanelControllerLifecycleAdmission{.available = false};
                },
                [](PaneRequestId) { FAIL("unavailable deferred refresh must not be scheduled"); },
                [&](const PanelControllerLifecycleSubmissionResult &_resolution) {
                    ++resolution_count;
                    CHECK(_resolution.status == PanelControllerLifecycleSubmissionStatus::Rejected);
                    CHECK(_resolution.rejection_reason == PaneRejectionReason::Unavailable);
                    CHECK_FALSE(_resolution.rejection_error);
                });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
        });

        const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [] {}) ==
              PaneLifecycleProducer::FinishResult::Published);
        CHECK(resolution_count == 1);
    }

    SECTION("throwing admission probe preserves its typed diagnostic")
    {
        PanelControllerLifecycle lifecycle(PaneId{30}, MappedException);
        int resolution_count = 0;
        bool queued = false;
        const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
            if( queued || !std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
                return;
            queued = true;
            const auto deferred = lifecycle.SubmitRefresh(
                Refresh(),
                [](const PanelControllerLifecycleProbeContext &) -> PanelControllerLifecycleAdmission {
                    throw std::runtime_error("deferred probe failed");
                },
                [](PaneRequestId) { FAIL("failed deferred probe must not schedule work"); },
                [&](const PanelControllerLifecycleSubmissionResult &_resolution) {
                    ++resolution_count;
                    CHECK(_resolution.status == PanelControllerLifecycleSubmissionStatus::Rejected);
                    CHECK(_resolution.rejection_reason == PaneRejectionReason::Unavailable);
                    REQUIRE(_resolution.rejection_error);
                    CHECK(_resolution.rejection_error->technical_message == "deferred probe failed");
                });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
        });

        const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
        REQUIRE(first.request_id);
        CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [] {}) ==
              PaneLifecycleProducer::FinishResult::Published);
        CHECK(resolution_count == 1);
    }
}

TEST_CASE(PREFIX "re-probes deferred admission with the completed lifecycle tail identity")
{
    PanelControllerLifecycle lifecycle(PaneId{14}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    std::optional<PaneRequestId> completed;
    bool unavailable_at_drain = false;
    int probe_count = 0;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( completed && _event.request_id == *completed &&
            std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ) {
            const auto deferred = lifecycle.SubmitRefresh(
                Refresh(),
                [&](const PanelControllerLifecycleProbeContext &_context) {
                    ++probe_count;
                    CHECK_FALSE(_context.lifecycle_active_request);
                    CHECK(_context.lifecycle_tail_request == completed);
                    return PanelControllerLifecycleAdmission{.available = !unavailable_at_drain};
                },
                [](PaneRequestId) { FAIL("fresh unavailable probe must reject before scheduling"); });
            CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
            CHECK(probe_count == 0);
            unavailable_at_drain = true;
        }
    });

    const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(first.request_id);
    completed = first.request_id;
    CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [] {}) ==
          PaneLifecycleProducer::FinishResult::Published);

    CHECK(probe_count == 1);
    REQUIRE(events.size() == 3);
    CHECK(std::holds_alternative<PaneLifecycleRejected>(events.back().payload));
    CHECK(std::get<PaneLifecycleRejected>(events.back().payload).reason ==
          PaneRejectionReason::Unavailable);
}

TEST_CASE(PREFIX "continues draining FIFO after a deferred probe exception")
{
    PanelControllerLifecycle lifecycle(PaneId{22}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    bool queued = false;
    bool tail_scheduler_called = false;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( !queued && std::holds_alternative<PaneLifecycleCommitted>(_event.payload) ) {
            queued = true;
            CHECK(lifecycle.SubmitRefresh(
                      Refresh(),
                      [](const PanelControllerLifecycleProbeContext &)
                          -> PanelControllerLifecycleAdmission { throw std::runtime_error("probe failed"); },
                      [](PaneRequestId) { FAIL("failed probe must not schedule work"); })
                      .status == PanelControllerLifecycleSubmissionStatus::Deferred);
            CHECK(lifecycle.SubmitRefresh(
                      Refresh(), Allow(), [&](PaneRequestId) { tail_scheduler_called = true; })
                      .status == PanelControllerLifecycleSubmissionStatus::Deferred);
        }
    });

    const auto request = lifecycle.SubmitNavigation(
        Navigation("/fifo/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {});
    REQUIRE(request.request_id);
    CHECK(lifecycle.Commit(*request.request_id, PaneLifecycleCommitted{}, [] {}) ==
          PaneLifecycleProducer::FinishResult::Published);

    CHECK(tail_scheduler_called);
    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<PaneLifecycleRejected>(events[2].payload));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[3].payload));
}

TEST_CASE(PREFIX "serializes shutdown requested from Started and skips the scheduler")
{
    PanelControllerLifecycle lifecycle(PaneId{15}, MappedException);
    std::vector<PaneLifecycleEvent> events;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) )
            lifecycle.Shutdown();
    });

    const auto result = lifecycle.SubmitNavigation(
        Navigation("/shutdown/"), PaneNavigationExecution::Asynchronous, Allow(), [](PaneRequestId) {
            FAIL("shutdown request must suppress scheduling");
        });
    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Accepted);
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    CHECK_FALSE(lifecycle.Active());
}

TEST_CASE(PREFIX "survives callback-driven facade destruction and suppresses scheduling")
{
    std::vector<PaneLifecycleEvent> events;
    PanelControllerLifecycle::ObservationTicket observation;
    auto lifecycle = std::make_unique<PanelControllerLifecycle>(PaneId{16}, MappedException);
    observation = lifecycle->Observe([&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) )
            lifecycle.reset();
    });

    const auto result = lifecycle->SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {
        FAIL("destroyed facade must suppress scheduling");
    });
    CHECK(result.status == PanelControllerLifecycleSubmissionStatus::Accepted);
    CHECK_FALSE(lifecycle);
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    observation = {};
}

TEST_CASE(PREFIX "defers shutdown requested during mutation until Committed publication completes")
{
    PanelControllerLifecycle lifecycle(PaneId{17}, MappedException);
    int deferred_resolution_count = 0;
    std::vector<PaneLifecycleEvent> first_events;
    std::vector<PaneLifecycleEvent> second_events;
    const auto first_observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { first_events.emplace_back(_event); });
    const auto second_observation = lifecycle.Observe(
        [&](const PaneLifecycleEvent &_event) { second_events.emplace_back(_event); });
    const auto request = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(request.request_id);

    CHECK(lifecycle.Commit(*request.request_id,
                           PaneLifecycleCommitted{.controller_generation = 23},
                           [&] {
              const auto deferred =
                  lifecycle.SubmitRefresh(
                      Refresh(),
                      Allow(),
                      [](PaneRequestId) { FAIL("shutdown must preempt deferred scheduling"); },
                      [&](const PanelControllerLifecycleSubmissionResult &) {
                          ++deferred_resolution_count;
                      });
              CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
              lifecycle.Shutdown();
          }) == PaneLifecycleProducer::FinishResult::Published);
    REQUIRE(first_events.size() == 2);
    REQUIRE(second_events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(first_events[1].payload));
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(second_events[1].payload));
    CHECK(deferred_resolution_count == 0);
    CHECK_FALSE(lifecycle.Active());
    CHECK(lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {}).status ==
          PanelControllerLifecycleSubmissionStatus::Shutdown);
}

TEST_CASE(PREFIX "resolves the current deferred attempt when its probe requests shutdown")
{
    PanelControllerLifecycle lifecycle(PaneId{31}, MappedException);
    int resolution_count = 0;
    bool queued = false;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        if( queued || !std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
            return;
        queued = true;
        const auto deferred = lifecycle.SubmitRefresh(
            Refresh(),
            [&](const PanelControllerLifecycleProbeContext &) {
                lifecycle.Shutdown();
                return PanelControllerLifecycleAdmission{};
            },
            [](PaneRequestId) { FAIL("shutdown during probe must suppress scheduling"); },
            [&](const PanelControllerLifecycleSubmissionResult &_resolution) {
                ++resolution_count;
                CHECK(_resolution.status == PanelControllerLifecycleSubmissionStatus::Shutdown);
                CHECK_FALSE(_resolution.request_id);
            });
        CHECK(deferred.status == PanelControllerLifecycleSubmissionStatus::Deferred);
    });

    const auto first = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(first.request_id);
    CHECK(lifecycle.Commit(*first.request_id, PaneLifecycleCommitted{}, [] {}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(resolution_count == 1);
    CHECK(detail::PanelControllerLifecycleTestAccess::ShutdownPerformed(lifecycle));
}

TEST_CASE(PREFIX "retries producer shutdown until it completes")
{
    PanelControllerLifecycle lifecycle(PaneId{24}, MappedException);
    bool inject_failure = true;
    const auto observation = lifecycle.Observe([&](const PaneLifecycleEvent &_event) {
        if( inject_failure && std::holds_alternative<PaneLifecycleCancelled>(_event.payload) ) {
            inject_failure = false;
            throw std::runtime_error("injected shutdown propagation failure");
        }
    });
    const auto request = lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {});
    REQUIRE(request.request_id);
    CHECK_FALSE(detail::PanelControllerLifecycleTestAccess::ShutdownPerformed(lifecycle));

    CHECK_THROWS_AS(lifecycle.Shutdown(), std::runtime_error);
    CHECK_FALSE(detail::PanelControllerLifecycleTestAccess::ShutdownPerformed(lifecycle));
    CHECK_FALSE(lifecycle.Active());

    CHECK_NOTHROW(lifecycle.Shutdown());
    CHECK(detail::PanelControllerLifecycleTestAccess::ShutdownPerformed(lifecycle));
    CHECK(lifecycle.SubmitRefresh(Refresh(), Allow(), [](PaneRequestId) {}).status ==
          PanelControllerLifecycleSubmissionStatus::Shutdown);
}
