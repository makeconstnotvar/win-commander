// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationCenterModel.h>

#include "../source/OperationCenterModelTesting.h"

#include <catch2/catch_all.hpp>

#include <chrono>
#include <concepts>
#include <type_traits>

using namespace nc::ops;
using namespace std::chrono_literals;

namespace {

OperationPlan OperationCenterModelUTPlan(std::string _id = "plan-1")
{
    auto plan = OperationPlan::Create({.plan_id = std::move(_id),
                                       .type = OperationPlanType::Copy,
                                       .sources = {OperationPlanSourceInput{"native", "/source"}},
                                       .destination = OperationPlanDestinationInput{
                                           "native", "/destination", OperationPlanDestinationKind::Directory},
                                       .conflict_policy = OperationPlanConflictPolicy{
                                           OperationPlanConflictDecision::Ask,
                                           OperationPlanConflictScope::ThisItem},
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

template <class T>
concept OperationCenterModelUTExecutorAuthority = requires(T &_value) {
    _value.Operation();
    _value.Pool();
    _value.Callback();
    _value.Enqueue();
};

OperationPlan::TimePoint At(const int _seconds)
{
    return OperationPlan::TimePoint{1'700'000'000s + std::chrono::seconds{_seconds}};
}

template <class T, class U>
concept OperationCenterModelUTAdmissible = requires(T &_model, U &&_candidate, const OperationPlan &_plan) {
    _model.Admit(std::move(_candidate), _plan, At(0));
};

template <class T>
concept OperationCenterModelUTReservable = requires(T &_model) {
    _model.Reserve();
};

} // namespace

TEST_CASE("OperationCenterModel: opaque execution IDs are distinct and serializable", "[operation-center-model]")
{
    STATIC_REQUIRE_FALSE(std::default_initializable<OperationId>);
    STATIC_REQUIRE_FALSE(OperationCenterModelUTExecutorAuthority<OperationRecord>);

    OperationCenterModel model;
    STATIC_REQUIRE_FALSE(OperationCenterModelUTReservable<OperationCenterModel>);
    const auto first = OperationCenterModelTesting::Reserve(model);
    const auto second = OperationCenterModelTesting::Reserve(model);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->Id() != second->Id());
    CHECK(first->Id().ToString() == "op-1");
    CHECK(second->Id().ToString() == "op-2");
    CHECK(OperationId::Parse(first->Id().ToString()) == first->Id());
    CHECK_FALSE(OperationId::Parse(""));
    CHECK_FALSE(OperationId::Parse("op-0"));
    CHECK_FALSE(OperationId::Parse("op-01"));
    CHECK_FALSE(OperationId::Parse("operation-1"));
    CHECK_FALSE(OperationId::Parse("op-1x"));
}

TEST_CASE("OperationCenterModel: only model-issued reservations can publish immutable records", "[operation-center-model]")
{
    STATIC_REQUIRE_FALSE(OperationCenterModelUTAdmissible<OperationCenterModel, OperationId>);
    STATIC_REQUIRE_FALSE(std::copy_constructible<OperationCenterModel::Reservation>);
    STATIC_REQUIRE(std::move_constructible<OperationCenterModel::Reservation>);

    OperationCenterModel model;
    const auto plan = OperationCenterModelUTPlan();
    auto reserved = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reserved);
    const auto id = reserved->Id();
    const auto parsed = OperationId::Parse(id.ToString());
    REQUIRE(parsed);
    CHECK(*parsed == id);

    const auto admitted = OperationCenterModelTesting::Admit(model, std::move(*reserved), plan, At(0));
    REQUIRE(admitted);
    CHECK(admitted->operation_id == id);
    CHECK(admitted->plan_id.Value() == "plan-1");
    CHECK(admitted->operation_type == OperationPlanType::Copy);
    CHECK(admitted->state == OperationRecordState::Queued);
    CHECK(admitted->revision == 1);
    CHECK(admitted->controls.can_cancel);
    CHECK_FALSE(admitted->controls.can_pause);
    CHECK_FALSE(admitted->started_at);
    CHECK_FALSE(admitted->finished_at);

    const auto duplicate = OperationCenterModelTesting::Admit(model, std::move(*reserved), plan, At(1));
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == OperationCenterModelErrorCode::UnreservedOperationId);
    CHECK(model.Snapshot() == std::vector<OperationRecord>{*admitted});
}

TEST_CASE("OperationCenterModel: revisioned lifecycle preserves finalization ordering", "[operation-center-model]")
{
    OperationCenterModel model;
    auto reservation = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reservation);
    const auto id = reservation->Id();
    const auto admitted =
        OperationCenterModelTesting::Admit(model, std::move(*reservation), OperationCenterModelUTPlan(), At(0));
    REQUIRE(admitted);

    const auto running = model.Transition(id, admitted->revision, OperationRecordState::Running, At(1));
    REQUIRE(running);
    CHECK(running->revision == 2);
    CHECK(running->started_at == std::optional{At(1)});
    CHECK(running->controls.can_pause);
    CHECK(running->controls.can_cancel);

    const auto stale = model.Transition(id, admitted->revision, OperationRecordState::Paused, At(2));
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == OperationCenterModelErrorCode::StaleRevision);
    CHECK(stale.error().current_revision == std::optional<uint64_t>{running->revision});

    const auto invalid_terminal = model.Transition(id, running->revision, OperationRecordState::Completed, At(2));
    REQUIRE_FALSE(invalid_terminal);
    CHECK(invalid_terminal.error().code == OperationCenterModelErrorCode::InvalidTransition);

    const auto finalizing = model.Transition(id, running->revision, OperationRecordState::Finalizing, At(2));
    REQUIRE(finalizing);
    CHECK_FALSE(finalizing->controls.can_cancel);

    const auto completed = model.Transition(id, finalizing->revision, OperationRecordState::Completed, At(3));
    REQUIRE(completed);
    CHECK(completed->revision == 4);
    CHECK(completed->finished_at == std::optional{At(3)});
    CHECK_FALSE(completed->controls.can_cancel);
    CHECK_FALSE(model.Transition(id, completed->revision, OperationRecordState::Running, At(4)));
}

TEST_CASE("OperationCenterModel: snapshots retain values across later transitions", "[operation-center-model]")
{
    OperationCenterModel model;
    auto reservation = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reservation);
    const auto id = reservation->Id();
    const auto admitted =
        OperationCenterModelTesting::Admit(model, std::move(*reservation), OperationCenterModelUTPlan(), At(0));
    REQUIRE(admitted);
    const auto before = model.Snapshot();

    REQUIRE(model.Transition(id, admitted->revision, OperationRecordState::Cancelling, At(1)));
    const auto after = model.Snapshot();
    REQUIRE(before.size() == 1);
    REQUIRE(after.size() == 1);
    CHECK(before.front().state == OperationRecordState::Queued);
    CHECK(before.front().revision == 1);
    CHECK(after.front().state == OperationRecordState::Cancelling);
    CHECK(after.front().revision == 2);
}

TEST_CASE("OperationCenterModel: observers see every accepted change and nothing else",
          "[operation-center-model]")
{
    OperationCenterModel model;
    unsigned notifications = 0;
    const auto ticket = model.ObserveChanges([&notifications] { ++notifications; });

    auto reservation = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reservation);
    // Reserving allocates nothing observable: no record exists yet for a consumer to render.
    CHECK(notifications == 0);

    const auto id = reservation->Id();
    const auto admitted =
        OperationCenterModelTesting::Admit(model, std::move(*reservation), OperationCenterModelUTPlan(), At(0));
    REQUIRE(admitted);
    CHECK(notifications == 1);

    const auto running = model.Transition(id, admitted->revision, OperationRecordState::Running, At(1));
    REQUIRE(running);
    CHECK(notifications == 2);

    // A rejected transition changed nothing, so it must not wake a consumer into a pointless redraw.
    const auto stale = model.Transition(id, admitted->revision, OperationRecordState::Running, At(2));
    REQUIRE_FALSE(stale);
    CHECK(notifications == 2);

    const auto invalid = model.Transition(id, running->revision, OperationRecordState::Queued, At(3));
    REQUIRE_FALSE(invalid);
    CHECK(notifications == 2);

    const auto unknown = model.Transition(OperationCenterModelTesting::Reserve(model)->Id(),
                                          1,
                                          OperationRecordState::Running,
                                          At(4));
    REQUIRE_FALSE(unknown);
    CHECK(notifications == 2);

    // Reads never notify.
    [[maybe_unused]] const auto snapshot = model.Snapshot();
    [[maybe_unused]] const auto found = model.Find(id);
    CHECK(notifications == 2);
}

TEST_CASE("OperationCenterModel: an observer may read the model without deadlocking",
          "[operation-center-model]")
{
    // The whole point of being notified is to re-read, so the callback must run with the model's
    // lock released. Firing under the lock would hang here rather than fail.
    OperationCenterModel model;
    std::vector<OperationRecord> observed;
    const auto ticket = model.ObserveChanges([&model, &observed] { observed = model.Snapshot(); });

    auto reservation = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reservation);
    const auto id = reservation->Id();
    const auto admitted =
        OperationCenterModelTesting::Admit(model, std::move(*reservation), OperationCenterModelUTPlan(), At(0));
    REQUIRE(admitted);

    // The snapshot taken inside the callback already contains the change that triggered it.
    REQUIRE(observed.size() == 1);
    CHECK(observed[0].operation_id == id);
    CHECK(observed[0].state == OperationRecordState::Queued);

    REQUIRE(model.Transition(id, admitted->revision, OperationRecordState::Running, At(1)));
    REQUIRE(observed.size() == 1);
    CHECK(observed[0].state == OperationRecordState::Running);
    CHECK(observed[0].revision == 2);
}

TEST_CASE("OperationCenterModel: observation stops with its ticket and survives the model outliving it",
          "[operation-center-model]")
{
    OperationCenterModel model;
    unsigned first = 0;
    unsigned second = 0;
    auto first_ticket = model.ObserveChanges([&first] { ++first; });
    const auto second_ticket = model.ObserveChanges([&second] { ++second; });

    auto reservation = OperationCenterModelTesting::Reserve(model);
    REQUIRE(reservation);
    const auto id = reservation->Id();
    const auto admitted =
        OperationCenterModelTesting::Admit(model, std::move(*reservation), OperationCenterModelUTPlan(), At(0));
    REQUIRE(admitted);
    CHECK(first == 1);
    CHECK(second == 1);

    // Releasing one ticket must retire only that observer; a consumer closing its panel cannot
    // silently stop everyone else's updates.
    first_ticket = {};
    REQUIRE(model.Transition(id, admitted->revision, OperationRecordState::Running, At(1)));
    CHECK(first == 1);
    CHECK(second == 2);
}

TEST_CASE("OperationCenterModel: an observer outliving the model is not invoked afterwards",
          "[operation-center-model]")
{
    unsigned notifications = 0;
    nc::ops::OperationCenterModel::ObservationTicket ticket;
    {
        OperationCenterModel model;
        ticket = model.ObserveChanges([&notifications] { ++notifications; });
        auto reservation = OperationCenterModelTesting::Reserve(model);
        REQUIRE(reservation);
        REQUIRE(OperationCenterModelTesting::Admit(
            model, std::move(*reservation), OperationCenterModelUTPlan(), At(0)));
        CHECK(notifications == 1);
    }
    // The ticket now refers to a destroyed model; releasing it must not touch freed memory. A
    // sanitizer run is what actually proves this, which is why this slice runs one.
    ticket = {};
    CHECK(notifications == 1);
}
