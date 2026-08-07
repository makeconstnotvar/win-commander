// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/ExplorerViewSettingsBinding.h>


namespace {

using nc::core::PaneId;
using nc::core::PaneLoadPhase;
using nc::explorer::ExplorerViewSettings;
using nc::explorer::ExplorerViewSettingsBindingAction;
using nc::explorer::ExplorerViewSettingsBindingPolicy;
using nc::explorer::ExplorerViewSettingsObservation;

ExplorerViewSettings Settings(const int _slot)
{
    ExplorerViewSettings result;
    result.layout_slot = _slot;
    return result;
}

ExplorerViewSettingsObservation Observation(const uint64_t _revision,
                                            const unsigned long _generation = 1,
                                            const void *_host = reinterpret_cast<const void *>(0x1234),
                                            std::string _path = "/folder/",
                                            const int _slot = 1)
{
    return {.pane_id = PaneId{41},
            .observation_sequence = _revision,
            .revision = _revision,
            .location_generation = _generation,
            .load_phase = PaneLoadPhase::Loaded,
            .is_uniform = true,
            .host_identity = _host,
            .path = std::move(_path),
            .settings = Settings(_slot)};
}

} // namespace

#define PREFIX "nc::explorer::ExplorerViewSettingsBindingPolicy "

TEST_CASE(PREFIX "requires an exact complete loaded uniform observation")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};

    auto wrong_pane = Observation(1);
    wrong_pane.pane_id = PaneId{42};
    CHECK(policy.Observe(wrong_pane) == ExplorerViewSettingsBindingAction::Rejected);

    auto loading = Observation(1);
    loading.load_phase = PaneLoadPhase::Loading;
    CHECK(policy.Observe(loading) == ExplorerViewSettingsBindingAction::Rejected);

    auto non_uniform = Observation(1);
    non_uniform.is_uniform = false;
    CHECK(policy.Observe(non_uniform) == ExplorerViewSettingsBindingAction::Rejected);

    auto absent_host = Observation(1);
    absent_host.host_identity = nullptr;
    CHECK(policy.Observe(absent_host) == ExplorerViewSettingsBindingAction::Rejected);

    auto non_directory_path = Observation(1);
    non_directory_path.path = "/folder";
    CHECK(policy.Observe(non_directory_path) == ExplorerViewSettingsBindingAction::Rejected);

    auto incomplete_settings = Observation(1);
    incomplete_settings.settings.reset();
    CHECK(policy.Observe(incomplete_settings) == ExplorerViewSettingsBindingAction::Rejected);

    auto refreshing = Observation(1);
    refreshing.load_phase = PaneLoadPhase::Refreshing;
    CHECK(policy.Observe(refreshing) == ExplorerViewSettingsBindingAction::LoadLocation);
}

TEST_CASE(PREFIX "loads once then stores only a changed complete value")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(7);
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.AcceptCurrent(initial));
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::None);

    const auto changed = Observation(8, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 3);
    CHECK(policy.Observe(changed) == ExplorerViewSettingsBindingAction::StoreCurrent);
    CHECK(policy.Observe(changed) == ExplorerViewSettingsBindingAction::StoreCurrent);
    REQUIRE(policy.AcceptCurrent(changed));
    CHECK(policy.Observe(changed) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "retries an unacknowledged initial store")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(9);
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::StoreCurrent);
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::StoreCurrent);
    REQUIRE(policy.AcceptCurrent(initial));
    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "treats generation host instance and path as one exact location key")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    auto current = Observation(10);
    REQUIRE(policy.Observe(current) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.AcceptCurrent(current));

    CHECK(policy.Observe(Observation(11, 2)) == ExplorerViewSettingsBindingAction::LoadLocation);
    auto host_changed = Observation(12, 2, reinterpret_cast<const void *>(0x5678));
    CHECK(policy.Observe(host_changed) == ExplorerViewSettingsBindingAction::LoadLocation);
    auto path_changed = Observation(13, 2, reinterpret_cast<const void *>(0x5678), "/other/");
    CHECK(policy.Observe(path_changed) == ExplorerViewSettingsBindingAction::LoadLocation);
}

TEST_CASE(PREFIX "rejects a stale revision before it can restore an old location")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    auto current = Observation(20, 4, reinterpret_cast<const void *>(0x1234), "/new/");
    REQUIRE(policy.Observe(current) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.AcceptCurrent(current));

    const auto stale = Observation(19, 3, reinterpret_cast<const void *>(0x1234), "/old/");
    CHECK(policy.Observe(stale) == ExplorerViewSettingsBindingAction::Rejected);
    CHECK(policy.Observe(current) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "fences the snapshot which initiated a restore")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(30, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 1);
    REQUIRE(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.BeginRestore(Settings(4)));

    CHECK(policy.Observe(initial) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "settles a restore from a full-layout sample even when PaneStore revision is unchanged")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(30, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 1);
    REQUIRE(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.BeginRestore(Settings(4)));

    const auto settled = Observation(31, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 4);
    auto context_sample = settled;
    context_sample.revision = 30;
    CHECK(policy.Observe(context_sample) == ExplorerViewSettingsBindingAction::RestoreSettled);

    auto duplicate = context_sample;
    duplicate.observation_sequence = 32;
    CHECK(policy.Observe(duplicate) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "exposes a post-fence restore divergence for conservative persistence")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(40, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 1);
    REQUIRE(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    REQUIRE(policy.BeginRestore(Settings(4)));

    const auto diverged = Observation(41, 1, reinterpret_cast<const void *>(0x1234), "/folder/", 2);
    CHECK(policy.Observe(diverged) == ExplorerViewSettingsBindingAction::RestoreDiverged);
    CHECK(policy.Observe(diverged) == ExplorerViewSettingsBindingAction::StoreCurrent);
    REQUIRE(policy.AcceptCurrent(diverged));
    CHECK(policy.Observe(diverged) == ExplorerViewSettingsBindingAction::None);
}

TEST_CASE(PREFIX "cannot acknowledge another revision or location")
{
    ExplorerViewSettingsBindingPolicy policy{PaneId{41}};
    const auto initial = Observation(50);
    REQUIRE(policy.Observe(initial) == ExplorerViewSettingsBindingAction::LoadLocation);
    CHECK_FALSE(policy.AcceptCurrent(Observation(49)));
    CHECK_FALSE(policy.AcceptCurrent(Observation(50, 2)));
    CHECK_FALSE(policy.AcceptCurrent(Observation(50, 1, reinterpret_cast<const void *>(0x5678))));
    CHECK_FALSE(policy.AcceptCurrent(Observation(50, 1, reinterpret_cast<const void *>(0x1234), "/other/")));
    CHECK(policy.AcceptCurrent(initial));
}

#undef PREFIX
