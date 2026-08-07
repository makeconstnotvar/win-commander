// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/VisualState/ExplorerOperationProgressModel.h>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace nc::core;
using namespace std::chrono_literals;
using Catch::Approx;

#define PREFIX "nc::core::ExplorerOperationProgressModel "

namespace {

ExplorerOperationProgressInput Input(const uint64_t _order,
                                     std::string _title,
                                     const ExplorerOperationLifecycle _lifecycle)
{
    ExplorerOperationProgressInput input;
    input.stable_order = _order;
    input.title = std::move(_title);
    input.lifecycle = _lifecycle;
    return input;
}

} // namespace

TEST_CASE(PREFIX "has no projection for an empty operation set")
{
    CHECK_FALSE(ExplorerOperationProgressModel::Build({}));
}

TEST_CASE(PREFIX "copies and normalizes determinate byte progress")
{
    ExplorerOperationProgressInput input = Input(3, "Copying fixtures", ExplorerOperationLifecycle::Running);
    input.current_item_path = "/fixtures/source/current.bin";
    input.bytes = {.processed = 600, .total = 1'000, .speed_per_second = 125., .eta = 3200ms};

    const auto projected = ExplorerOperationProgressModel::Build(std::span{&input, 1});
    REQUIRE(projected);
    CHECK(projected->title == "Copying fixtures");
    CHECK(projected->current_item_path == "/fixtures/source/current.bin");
    CHECK(projected->lifecycle == ExplorerOperationLifecycle::Running);
    CHECK(projected->unit == ExplorerOperationProgressUnit::Bytes);
    CHECK(projected->processed == 600);
    CHECK(projected->total == 1'000);
    CHECK(projected->fraction == Approx(0.6));
    CHECK(projected->speed_per_second == 125.);
    CHECK(projected->eta == 4s);
    CHECK_FALSE(projected->indeterminate);
    CHECK(projected->active_count == 1);
    CHECK(projected->additional_count == 0);

    input.title = "Mutated after projection";
    input.current_item_path = "/mutated";
    CHECK(projected->title == "Copying fixtures");
    CHECK(projected->current_item_path == "/fixtures/source/current.bin");
}

TEST_CASE(PREFIX "uses the meaningful alternate source and supports item progress")
{
    ExplorerOperationProgressInput input = Input(0, "Deleting", ExplorerOperationLifecycle::Running);
    input.preferred_unit = ExplorerOperationProgressUnit::Bytes;
    input.items = {.processed = 2, .total = 8, .speed_per_second = 1.5, .eta = 4s};

    const auto projected = ExplorerOperationProgressModel::Build(std::span{&input, 1});
    REQUIRE(projected);
    CHECK(projected->unit == ExplorerOperationProgressUnit::Items);
    CHECK(projected->processed == 2);
    CHECK(projected->total == 8);
    CHECK(projected->fraction == Approx(0.25));
    CHECK(projected->speed_per_second == 1.5);
    CHECK(projected->eta == 4s);
}

TEST_CASE(PREFIX "keeps unknown totals indeterminate and preserves processed work")
{
    ExplorerOperationProgressInput input = Input(0, "Scanning", ExplorerOperationLifecycle::Running);
    input.current_item_path = std::string{};
    input.bytes = {.processed = 4'096, .total = 0, .speed_per_second = 512., .eta = std::nullopt};

    const auto projected = ExplorerOperationProgressModel::Build(std::span{&input, 1});
    REQUIRE(projected);
    CHECK(projected->processed == 4'096);
    CHECK(projected->total == 0);
    CHECK(projected->fraction == 0.);
    CHECK(projected->indeterminate);
    REQUIRE(projected->current_item_path);
    CHECK(projected->current_item_path->empty());
    CHECK_FALSE(projected->eta);
}

TEST_CASE(PREFIX "clamps overflow progress and rejects invalid rate samples")
{
    const auto invalid_speeds = std::to_array<double>(
        {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()});
    for( const double speed : invalid_speeds ) {
        CAPTURE(speed);
        ExplorerOperationProgressInput input = Input(0, "Copying", ExplorerOperationLifecycle::Running);
        input.bytes = {.processed = std::numeric_limits<uint64_t>::max(),
                       .total = 12,
                       .speed_per_second = speed,
                       .eta = -1ns};

        const auto projected = ExplorerOperationProgressModel::Build(std::span{&input, 1});
        REQUIRE(projected);
        CHECK(projected->processed == 12);
        CHECK(projected->fraction == 1.);
        CHECK_FALSE(projected->speed_per_second);
        CHECK_FALSE(projected->eta);
        CHECK_FALSE(projected->indeterminate);
    }
}

TEST_CASE(PREFIX "chooses a deterministic primary operation by lifecycle then copied pool order")
{
    std::vector<ExplorerOperationProgressInput> inputs;
    inputs.emplace_back(Input(40, "Queued", ExplorerOperationLifecycle::Queued));
    inputs.emplace_back(Input(30, "Finalizing", ExplorerOperationLifecycle::Finalizing));
    inputs.emplace_back(Input(20, "Paused", ExplorerOperationLifecycle::Paused));
    inputs.emplace_back(Input(10, "Later running", ExplorerOperationLifecycle::Running));
    inputs.emplace_back(Input(5, "Earlier running", ExplorerOperationLifecycle::Running));
    inputs.emplace_back(Input(100, "Needs decision", ExplorerOperationLifecycle::WaitingForUser));

    const auto projected = ExplorerOperationProgressModel::Build(inputs);
    REQUIRE(projected);
    CHECK(projected->title == "Needs decision");
    CHECK_FALSE(projected->current_item_path);
    CHECK(projected->lifecycle == ExplorerOperationLifecycle::WaitingForUser);
    CHECK(projected->active_count == 6);
    CHECK(projected->additional_count == 5);

    inputs.pop_back();
    const auto without_waiting = ExplorerOperationProgressModel::Build(inputs);
    REQUIRE(without_waiting);
    CHECK(without_waiting->title == "Earlier running");
    CHECK(without_waiting->lifecycle == ExplorerOperationLifecycle::Running);
}

TEST_CASE(PREFIX "preserves exact large counters without overflowing fraction")
{
    ExplorerOperationProgressInput input = Input(0, "Large copy", ExplorerOperationLifecycle::Finalizing);
    input.bytes = {.processed = std::numeric_limits<uint64_t>::max() - 1,
                   .total = std::numeric_limits<uint64_t>::max(),
                   .speed_per_second = 1.};

    const auto projected = ExplorerOperationProgressModel::Build(std::span{&input, 1});
    REQUIRE(projected);
    CHECK(projected->processed == std::numeric_limits<uint64_t>::max() - 1);
    CHECK(projected->total == std::numeric_limits<uint64_t>::max());
    CHECK(std::isfinite(projected->fraction));
    CHECK(projected->fraction >= 0.);
    CHECK(projected->fraction <= 1.);
    CHECK(projected->lifecycle == ExplorerOperationLifecycle::Finalizing);
}
