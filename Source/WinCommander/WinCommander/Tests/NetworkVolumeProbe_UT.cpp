// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/NetworkVolumeProbe.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using nc::core::ClassifyNetworkVolume;
using nc::core::MayTouchSynchronously;
using nc::core::NetworkVolumeFacts;
using nc::core::NetworkVolumeProbeCache;
using nc::core::NetworkVolumeProbeResult;
using nc::core::NetworkVolumeState;
using nc::core::ProbeNetworkVolume;
using namespace std::chrono_literals;

/** Answers whatever it is told to, and counts how often it was asked. */
struct ScriptedProber {
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<NetworkVolumeProbeResult> answer =
        std::make_shared<NetworkVolumeProbeResult>(NetworkVolumeProbeResult{});

    NetworkVolumeProbeCache::Prober Handler() const
    {
        auto calls_copy = calls;
        auto answer_copy = answer;
        return [calls_copy, answer_copy](const std::string &) {
            ++*calls_copy;
            return *answer_copy;
        };
    }
};

} // namespace

#define PREFIX "nc::core::NetworkVolumeProbe "

TEST_CASE(PREFIX "answers from memory without asking again")
{
    // The point of the model: a volume's state is known before an access rather than discovered by
    // making one. Probing to draw a row would be exactly the touch being avoided.
    ScriptedProber prober;
    NetworkVolumeProbeCache cache{prober.Handler(), 10'000ms};

    CHECK(cache.Known("/Volumes/share", 0ms) == std::nullopt);
    CHECK(cache.NeedsRefresh("/Volumes/share", 0ms));
    CHECK(prober.calls->load() == 0); // asking what is known must never probe

    cache.Refresh("/Volumes/share", 0ms);
    CHECK(prober.calls->load() == 1);

    for( int i = 0; i < 5; ++i ) {
        const auto known = cache.Known("/Volumes/share", 1'000ms);
        REQUIRE(known);
        CHECK(known->answered);
    }
    CHECK_FALSE(cache.NeedsRefresh("/Volumes/share", 1'000ms));
    CHECK(prober.calls->load() == 1);
}

TEST_CASE(PREFIX "withholds an answer that has aged out instead of repeating it")
{
    // A `Responsive` from a minute ago would hide a mount that has died since - and hiding it is how
    // the synchronous touch happens anyway.
    ScriptedProber prober;
    NetworkVolumeProbeCache cache{prober.Handler(), 10'000ms};

    cache.Refresh("/Volumes/share", 0ms);
    CHECK(cache.Known("/Volumes/share", 10'000ms).has_value());
    CHECK(cache.Known("/Volumes/share", 10'001ms) == std::nullopt);
    CHECK(cache.NeedsRefresh("/Volumes/share", 10'001ms));

    // Refreshing brings it back, with the new answer.
    *prober.answer = NetworkVolumeProbeResult{.answered = false, .export_rejected = false};
    cache.Refresh("/Volumes/share", 10'001ms);
    const auto known = cache.Known("/Volumes/share", 10'001ms);
    REQUIRE(known);
    CHECK_FALSE(known->answered);
}

TEST_CASE(PREFIX "keeps mount points apart and forgets one that went away")
{
    ScriptedProber prober;
    NetworkVolumeProbeCache cache{prober.Handler(), 10'000ms};

    cache.Refresh("/Volumes/a", 0ms);
    *prober.answer = NetworkVolumeProbeResult{.answered = false, .export_rejected = true};
    cache.Refresh("/Volumes/b", 0ms);

    REQUIRE(cache.Known("/Volumes/a", 0ms));
    REQUIRE(cache.Known("/Volumes/b", 0ms));
    CHECK(cache.Known("/Volumes/a", 0ms)->answered);
    CHECK_FALSE(cache.Known("/Volumes/b", 0ms)->answered);
    CHECK(cache.Known("/Volumes/b", 0ms)->export_rejected);

    cache.Forget("/Volumes/a");
    CHECK(cache.Known("/Volumes/a", 0ms) == std::nullopt);
    CHECK(cache.Known("/Volumes/b", 0ms).has_value());
}

TEST_CASE(PREFIX "does not let a slow probe overwrite a fresher answer")
{
    // Two refreshes can be in flight at once; the one that started earlier may finish later, and its
    // answer is older news whatever order it arrives in.
    ScriptedProber prober;
    NetworkVolumeProbeCache cache{prober.Handler(), 10'000ms};

    *prober.answer = NetworkVolumeProbeResult{.answered = false, .export_rejected = false};
    cache.Refresh("/Volumes/share", 5'000ms);

    *prober.answer = NetworkVolumeProbeResult{.answered = true, .export_rejected = false};
    cache.Refresh("/Volumes/share", 1'000ms); // started earlier, landing later

    const auto known = cache.Known("/Volumes/share", 5'000ms);
    REQUIRE(known);
    CHECK_FALSE(known->answered);
}

TEST_CASE(PREFIX "reading what is known never blocks behind a probe in flight")
{
    // The prober is where the tens of seconds live. Holding the lock across it would stall every
    // drawing-thread read for exactly that long, reintroducing the problem one indirection away.
    std::atomic<bool> release{true};
    std::atomic<bool> probe_entered{false};
    NetworkVolumeProbeCache cache{
        [&](const std::string &) {
            probe_entered = true;
            while( !release.load() )
                std::this_thread::yield();
            return NetworkVolumeProbeResult{};
        },
        10'000ms};

    cache.Refresh("/Volumes/other", 0ms); // returns straight away: something to read later
    release = false;                      // and now hold the next one inside the prober
    probe_entered = false;
    std::thread worker{[&] { cache.Refresh("/Volumes/share", 0ms); }};
    while( !probe_entered.load() )
        std::this_thread::yield();

    // While that probe is stuck, reads still answer.
    CHECK(cache.Known("/Volumes/share", 0ms) == std::nullopt);
    CHECK(cache.Known("/Volumes/other", 0ms).has_value());
    CHECK(cache.NeedsRefresh("/Volumes/share", 0ms));

    release = true;
    worker.join();
    CHECK(cache.Known("/Volumes/share", 0ms).has_value());
}

TEST_CASE(PREFIX "gives up on a mount point that does not answer in time")
{
    // There is no way to interrupt a blocked stat(), so the wait is what has the deadline. This
    // exercises the real prober against a path that answers instantly, and against the budget.
    const NetworkVolumeProbeResult quick = ProbeNetworkVolume("/", 5'000ms);
    CHECK(quick.answered);

    // A budget nothing can meet: reported unresponsive rather than waited on.
    const NetworkVolumeProbeResult impatient = ProbeNetworkVolume("/", 0ms);
    CHECK_FALSE(impatient.answered);
    CHECK_FALSE(impatient.export_rejected);

    // A path that is not a mount point at all answers immediately, and says so.
    const NetworkVolumeProbeResult missing = ProbeNetworkVolume("/nonexistent-mount-point-for-tests", 5'000ms);
    CHECK_FALSE(missing.answered);
    CHECK(missing.export_rejected);
}

TEST_CASE(PREFIX "feeds the state a drawing thread is allowed to act on")
{
    ScriptedProber prober;
    NetworkVolumeProbeCache cache{prober.Handler(), 10'000ms};
    *prober.answer = NetworkVolumeProbeResult{.answered = false, .export_rejected = false};
    cache.Refresh("/Volumes/share", 0ms);

    const auto known = cache.Known("/Volumes/share", 0ms);
    REQUIRE(known);
    const NetworkVolumeFacts facts{.is_network_mount = true,
                                   .is_mounted = true,
                                   .last_probe_answered = known->answered,
                                   .export_rejected = known->export_rejected};
    CHECK(ClassifyNetworkVolume(facts) == NetworkVolumeState::Unresponsive);
    CHECK_FALSE(MayTouchSynchronously(ClassifyNetworkVolume(facts)));
}

#undef PREFIX
