// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/States/FilePanels/PanelHistory.h>

#include <VFS/VFSListing.h>
#include <VFS/VFSListingInput.h>
#include <dirent.h>
#include <sys/stat.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using nc::panel::History;

VFSListingPtr UniformListing(std::string _directory)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.filenames.emplace_back("entry");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

void CheckState(const History &_history,
                const bool _back,
                const bool _forward,
                const std::optional<History::EntryId> _current_entry_id)
{
    const History::NavigationState state = _history.GetNavigationState();
    CHECK(state.availability.can_go_back == _back);
    CHECK(state.availability.can_go_forward == _forward);
    CHECK(state.current_entry_id == _current_entry_id);
    CHECK(_history.GetNavigationAvailability() == state.availability);
    CHECK(_history.CanMoveBack() == _back);
    CHECK(_history.CanMoveForth() == _forward);
}

History::EntryId CurrentId(const History &_history)
{
    const auto id = _history.GetNavigationState().current_entry_id;
    REQUIRE(id);
    return *id;
}

class NavigationStateRecorder
{
public:
    explicit NavigationStateRecorder(History &_history) : m_History(_history)
    {
        m_History.SetNavigationStateChangeCallback([this] {
            m_Observed.emplace_back(m_History.GetNavigationState());
        });
    }

    [[nodiscard]] size_t Calls() const noexcept { return m_Observed.size(); }

    void CheckLast(const bool _back,
                   const bool _forward,
                   const std::optional<History::EntryId> _current_entry_id) const
    {
        REQUIRE_FALSE(m_Observed.empty());
        CHECK(m_Observed.back().availability.can_go_back == _back);
        CHECK(m_Observed.back().availability.can_go_forward == _forward);
        CHECK(m_Observed.back().current_entry_id == _current_entry_id);
    }

private:
    History &m_History;
    std::vector<History::NavigationState> m_Observed;
};

} // namespace

#define PREFIX "PanelHistory navigation state "

TEST_CASE(PREFIX "mints stable identities and keeps duplicate puts silent")
{
    const VFSListingPtr first = UniformListing("/first/");
    const VFSListingPtr second = UniformListing("/second/");
    History history;
    NavigationStateRecorder recorder{history};

    CheckState(history, false, false, std::nullopt);

    history.Put(*first);
    const History::EntryId first_id = CurrentId(history);
    CHECK(first_id != 0);
    CheckState(history, false, false, first_id);
    CHECK(recorder.Calls() == 1);
    recorder.CheckLast(false, false, first_id);

    history.Put(*first);
    CheckState(history, false, false, first_id);
    CHECK(recorder.Calls() == 1);

    history.Put(*second);
    const History::EntryId second_id = CurrentId(history);
    CHECK(second_id > first_id);
    CheckState(history, true, false, second_id);
    CHECK(recorder.Calls() == 2);

    history.MoveBack();
    CheckState(history, false, true, first_id);
    CHECK(recorder.Calls() == 3);

    // Loading the selected promise records the same playback entry and retains its identity.
    history.Put(*first);
    CheckState(history, false, true, first_id);
    CHECK(recorder.Calls() == 3);

    history.MoveForth();
    CheckState(history, true, false, second_id);
    CHECK(recorder.Calls() == 4);

    CHECK_THROWS_AS(history.MoveForth(), std::logic_error);
    CHECK(recorder.Calls() == 4);
}

TEST_CASE(PREFIX "publishes identity-only playback changes and a fresh branch identity")
{
    const VFSListingPtr first = UniformListing("/first/");
    const VFSListingPtr second = UniformListing("/second/");
    const VFSListingPtr third = UniformListing("/third/");
    const VFSListingPtr fourth = UniformListing("/fourth/");
    const VFSListingPtr branch = UniformListing("/branch/");
    History history;
    NavigationStateRecorder recorder{history};

    history.Put(*first);
    const History::EntryId first_id = CurrentId(history);
    history.Put(*second);
    const History::EntryId second_id = CurrentId(history);
    history.Put(*third);
    const History::EntryId third_id = CurrentId(history);
    history.Put(*fourth);
    const History::EntryId fourth_id = CurrentId(history);
    CHECK(recorder.Calls() == 4);

    history.MoveBack();
    CheckState(history, true, true, third_id);
    CHECK(recorder.Calls() == 5);

    // Both booleans stay true; the current entry identity still changes exactly once.
    history.MoveBack();
    CheckState(history, true, true, second_id);
    CHECK(recorder.Calls() == 6);
    recorder.CheckLast(true, true, second_id);

    history.MoveForth();
    CheckState(history, true, true, third_id);
    CHECK(recorder.Calls() == 7);

    history.Put(*third);
    CheckState(history, true, true, third_id);
    CHECK(recorder.Calls() == 7);

    history.Put(*branch);
    const History::EntryId branch_id = CurrentId(history);
    CHECK(branch_id > fourth_id);
    CHECK(branch_id > first_id);
    CheckState(history, true, false, branch_id);
    CHECK(history.Length() == 4);
    CHECK(history.IsRecording());
    CHECK(recorder.Calls() == 8);
}

TEST_CASE(PREFIX "RewindAt reports only effective current-state changes")
{
    const VFSListingPtr first = UniformListing("/first/");
    const VFSListingPtr second = UniformListing("/second/");
    const VFSListingPtr third = UniformListing("/third/");
    History history;
    history.Put(*first);
    const History::EntryId first_id = CurrentId(history);
    history.Put(*second);
    const History::EntryId second_id = CurrentId(history);
    history.Put(*third);
    const History::EntryId third_id = CurrentId(history);
    NavigationStateRecorder recorder{history};

    REQUIRE(history.RewindAt(1) != nullptr);
    CheckState(history, true, true, second_id);
    CHECK(recorder.Calls() == 1);

    REQUIRE(history.RewindAt(1) != nullptr);
    CheckState(history, true, true, second_id);
    CHECK(recorder.Calls() == 1);

    REQUIRE(history.RewindAt(0) != nullptr);
    CheckState(history, false, true, first_id);
    CHECK(recorder.Calls() == 2);

    CHECK(history.RewindAt(3) == nullptr);
    CheckState(history, false, true, first_id);
    CHECK(recorder.Calls() == 2);

    REQUIRE(history.RewindAt(2) != nullptr);
    CheckState(history, true, false, third_id);
    CHECK(recorder.Calls() == 3);
}

TEST_CASE(PREFIX "contains observer exceptions after committing identity changes")
{
    const VFSListingPtr first = UniformListing("/first/");
    const VFSListingPtr second = UniformListing("/second/");
    History history;
    history.Put(*first);
    const History::EntryId first_id = CurrentId(history);

    int calls = 0;
    history.SetNavigationStateChangeCallback([&] {
        ++calls;
        throw std::runtime_error("test observer failure");
    });

    CHECK_NOTHROW(history.Put(*second));
    const History::EntryId second_id = CurrentId(history);
    CHECK(second_id > first_id);
    CheckState(history, true, false, second_id);
    CHECK(calls == 1);

    CHECK_NOTHROW(history.Put(*second));
    CheckState(history, true, false, second_id);
    CHECK(calls == 1);

    CHECK_NOTHROW(history.MoveBack());
    CheckState(history, false, true, first_id);
    CHECK(calls == 2);
}

TEST_CASE(PREFIX "retains entry identities across bounded trimming")
{
    History history;
    History::EntryId previous_id = 0;
    for( int index = 0; index < 129; ++index ) {
        history.Put(*UniformListing("/entry-" + std::to_string(index) + "/"));
        const History::EntryId current_id = CurrentId(history);
        CHECK(current_id > previous_id);
        previous_id = current_id;
    }

    CHECK(history.Length() == 128);
    REQUIRE(history.RewindAt(0) != nullptr);
    CheckState(history, false, true, History::EntryId{2});
    REQUIRE(history.RewindAt(127) != nullptr);
    CheckState(history, true, false, History::EntryId{129});

    history.Put(*UniformListing("/branch-after-trim/"));
    CheckState(history, true, false, History::EntryId{130});
    CHECK(history.Length() == 128);
}

#undef PREFIX
