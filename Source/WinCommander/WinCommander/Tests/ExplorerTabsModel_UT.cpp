// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Pane/ExplorerTabsModel.h>
#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

using namespace nc::core;

struct ObservedState final {
    std::vector<PaneId> panes;
    PaneId active;
    size_t active_index = 0;

    bool operator==(const ObservedState &) const noexcept = default;
};

ObservedState Observe(const ExplorerTabsModel &_model)
{
    return {
        .panes = std::vector<PaneId>(_model.Panes().begin(), _model.Panes().end()),
        .active = _model.Active(),
        .active_index = _model.ActiveIndex(),
    };
}

ExplorerTabsModel Model(const PaneId _initial = PaneId{1})
{
    auto model = ExplorerTabsModel::Create(_initial);
    REQUIRE(model);
    return std::move(*model);
}

void CheckFailure(const ExplorerTabsMutationResult &_result, const ExplorerTabsFailure _failure)
{
    REQUIRE_FALSE(_result);
    CHECK(_result.error() == _failure);
}

void CheckPanes(const ExplorerTabsModel &_model, const std::initializer_list<PaneId> _expected)
{
    CHECK(std::ranges::equal(_model.Panes(), _expected));
}

} // namespace

#define PREFIX "nc::core::ExplorerTabsModel "

TEST_CASE(PREFIX "creates one active tab and rejects a zero identity")
{
    const auto invalid = ExplorerTabsModel::Create(PaneId{});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == ExplorerTabsFailure::ZeroPaneId);

    const auto model = Model(PaneId{41});
    CheckPanes(model, {PaneId{41}});
    CHECK(model.Active() == PaneId{41});
    CHECK(model.ActiveIndex() == 0);
    CHECK(model.Size() == 1);
}

TEST_CASE(PREFIX "appends and inserts unique tabs at both inclusive boundaries")
{
    auto model = Model();
    REQUIRE(model.Append(PaneId{2}));
    REQUIRE(model.Insert(0, PaneId{3}));
    REQUIRE(model.Insert(model.Size(), PaneId{4}));

    CheckPanes(model, {PaneId{3}, PaneId{1}, PaneId{2}, PaneId{4}});
    CHECK(model.Active() == PaneId{4});
    CHECK(model.ActiveIndex() == 3);
}

TEST_CASE(PREFIX "atomically rejects zero duplicate missing and out-of-range mutations")
{
    auto model = Model();
    REQUIRE(model.Append(PaneId{2}));
    const ObservedState baseline = Observe(model);

    CheckFailure(model.Activate(PaneId{}), ExplorerTabsFailure::ZeroPaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Activate(PaneId{9}), ExplorerTabsFailure::PaneNotFound);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Append(PaneId{}), ExplorerTabsFailure::ZeroPaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Append(PaneId{2}), ExplorerTabsFailure::DuplicatePaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Insert(0, PaneId{}), ExplorerTabsFailure::ZeroPaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Insert(model.Size() + 1, PaneId{3}), ExplorerTabsFailure::IndexOutOfRange);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Close(PaneId{}), ExplorerTabsFailure::ZeroPaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Close(PaneId{9}), ExplorerTabsFailure::PaneNotFound);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Reorder(PaneId{}, 0), ExplorerTabsFailure::ZeroPaneId);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Reorder(PaneId{9}, 0), ExplorerTabsFailure::PaneNotFound);
    CHECK(Observe(model) == baseline);
    CheckFailure(model.Reorder(PaneId{1}, model.Size()), ExplorerTabsFailure::IndexOutOfRange);
    CHECK(Observe(model) == baseline);
}

TEST_CASE(PREFIX "refuses to close the final tab atomically")
{
    auto model = Model(PaneId{7});
    const ObservedState baseline = Observe(model);

    CheckFailure(model.Close(PaneId{7}), ExplorerTabsFailure::LastTab);
    CHECK(Observe(model) == baseline);
}

TEST_CASE(PREFIX "keeps the active identity while closing inactive tabs")
{
    auto model = Model();
    REQUIRE(model.Append(PaneId{2}));
    REQUIRE(model.Append(PaneId{3}));
    REQUIRE(model.Activate(PaneId{2}));

    REQUIRE(model.Close(PaneId{1}));
    CheckPanes(model, {PaneId{2}, PaneId{3}});
    CHECK(model.Active() == PaneId{2});
    CHECK(model.ActiveIndex() == 0);

    REQUIRE(model.Close(PaneId{3}));
    CheckPanes(model, {PaneId{2}});
    CHECK(model.Active() == PaneId{2});
}

TEST_CASE(PREFIX "selects the right successor then the left successor when closing the active tab")
{
    auto model = Model();
    REQUIRE(model.Append(PaneId{2}));
    REQUIRE(model.Append(PaneId{3}));
    REQUIRE(model.Activate(PaneId{2}));

    REQUIRE(model.Close(PaneId{2}));
    CheckPanes(model, {PaneId{1}, PaneId{3}});
    CHECK(model.Active() == PaneId{3});
    CHECK(model.ActiveIndex() == 1);

    REQUIRE(model.Close(PaneId{3}));
    CheckPanes(model, {PaneId{1}});
    CHECK(model.Active() == PaneId{1});
    CHECK(model.ActiveIndex() == 0);
}

TEST_CASE(PREFIX "reorders active and inactive identities by final index")
{
    auto model = Model();
    REQUIRE(model.Append(PaneId{2}));
    REQUIRE(model.Append(PaneId{3}));
    REQUIRE(model.Append(PaneId{4}));
    REQUIRE(model.Activate(PaneId{2}));

    REQUIRE(model.Reorder(PaneId{4}, 0));
    CheckPanes(model, {PaneId{4}, PaneId{1}, PaneId{2}, PaneId{3}});
    CHECK(model.Active() == PaneId{2});
    CHECK(model.ActiveIndex() == 2);

    REQUIRE(model.Reorder(PaneId{2}, model.Size() - 1));
    CheckPanes(model, {PaneId{4}, PaneId{1}, PaneId{3}, PaneId{2}});
    CHECK(model.Active() == PaneId{2});
    CHECK(model.ActiveIndex() == 3);

    const ObservedState baseline = Observe(model);
    REQUIRE(model.Reorder(PaneId{2}, 3));
    CHECK(Observe(model) == baseline);
}

TEST_CASE(PREFIX "observation gate retires inactive and removed-tab callbacks")
{
    ExplorerTabObservationGate gate;
    const auto first = gate.Bind(PaneId{1});
    REQUIRE(first);
    CHECK(gate.Accepts(*first, PaneId{1}, PaneId{1}));
    CHECK_FALSE(gate.Accepts(*first, PaneId{2}, PaneId{1}));
    CHECK_FALSE(gate.Accepts(*first, PaneId{1}, PaneId{2}));

    const auto second = gate.Bind(PaneId{2});
    REQUIRE(second);
    CHECK_FALSE(gate.Accepts(*first, PaneId{2}, PaneId{1}));
    CHECK(gate.Accepts(*second, PaneId{2}, PaneId{2}));

    gate.Invalidate();
    CHECK_FALSE(gate.Accepts(*second, PaneId{2}, PaneId{2}));
}

TEST_CASE(PREFIX "observation gate rejects zero identity without advancing its token")
{
    ExplorerTabObservationGate gate;
    const auto active = gate.Bind(PaneId{7});
    REQUIRE(active);
    const auto invalid = gate.Bind(PaneId{});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == ExplorerTabsFailure::ZeroPaneId);
    CHECK(gate.Accepts(*active, PaneId{7}, PaneId{7}));
}

#undef PREFIX
