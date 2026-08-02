// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/VFSOperationPlanningAccessChecker.h>
#include <WinCommander/States/FilePanels/PanelController.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using nc::core::MakeVFSOperationPlanningAccessChecker;
using nc::ops::OperationPlanningAccessState;
using nc::ops::OperationPlanningPath;
using nc::ops::OperationPlanningRequiredAccess;

class AccessTestHost final : public nc::vfs::Host
{
public:
    explicit AccessTestHost(const char *_tag) : Host("/", nullptr, _tag) {}
};

class RecordingDirectoryAccessProvider final : public nc::panel::DirectoryAccessProvider
{
public:
    struct Call final {
        PanelController *panel = nullptr;
        std::string directory;
        VFSHost *host = nullptr;
    };

    bool HasAccess(PanelController *_panel,
                   const std::string &_directory_path,
                   VFSHost &_host) override
    {
        calls.emplace_back(Call{_panel, _directory_path, &_host});
        if( throw_from_has_access )
            throw std::runtime_error{"access checker failure"};
        return grants_access;
    }

    bool RequestAccessSync(PanelController *, const std::string &, VFSHost &) override
    {
        ++request_access_calls;
        return true;
    }

    bool grants_access = true;
    bool throw_from_has_access = false;
    int request_access_calls = 0;
    std::vector<Call> calls;
};

OperationPlanningAccessState StateOf(
    const nc::ops::VFSOperationPlanningProbes::AccessChecker &_checker,
    const OperationPlanningPath &_path,
    const OperationPlanningRequiredAccess _required,
    nc::vfs::Host &_host)
{
    const auto result = _checker(_path, _required, _host);
    REQUIRE(result);
    return result->state;
}

} // namespace

#define PREFIX "nc::core::VFSOperationPlanningAccessChecker "

TEST_CASE(PREFIX "projects each required access to the directory permission seam")
{
    RecordingDirectoryAccessProvider provider;
    const auto checker = MakeVFSOperationPlanningAccessChecker(provider);
    AccessTestHost host{"planning_access_host"};
    const OperationPlanningPath path{"bound-provider", "/parent/item"};
    constexpr std::array required_accesses{OperationPlanningRequiredAccess::Read,
                                           OperationPlanningRequiredAccess::Write,
                                           OperationPlanningRequiredAccess::ReplaceFile,
                                           OperationPlanningRequiredAccess::ReplaceDirectory};

    for( const auto required : required_accesses )
        CHECK(StateOf(checker, path, required, host) == OperationPlanningAccessState::Granted);

    REQUIRE(provider.calls.size() == required_accesses.size());
    CHECK(provider.calls[0].directory == "/parent");
    CHECK(provider.calls[1].directory == "/parent/item");
    CHECK(provider.calls[2].directory == "/parent");
    CHECK(provider.calls[3].directory == "/parent");
    for( const auto &call : provider.calls ) {
        CHECK(call.panel == nullptr);
        CHECK(call.host == &host);
    }
    CHECK(provider.request_access_calls == 0);
}

TEST_CASE(PREFIX "preserves the exact write path and handles parent root projection")
{
    RecordingDirectoryAccessProvider provider;
    const auto checker = MakeVFSOperationPlanningAccessChecker(provider);
    AccessTestHost native_host{"planning_access_native"};
    AccessTestHost remote_host{"planning_access_remote"};

    CHECK(StateOf(checker,
                  {"native", "/destination/"},
                  OperationPlanningRequiredAccess::Write,
                  native_host) == OperationPlanningAccessState::Granted);
    CHECK(StateOf(checker,
                  {"remote", "/item"},
                  OperationPlanningRequiredAccess::Read,
                  remote_host) == OperationPlanningAccessState::Granted);
    CHECK(StateOf(checker,
                  {"remote", "/"},
                  OperationPlanningRequiredAccess::ReplaceDirectory,
                  remote_host) == OperationPlanningAccessState::Granted);

    REQUIRE(provider.calls.size() == 3);
    CHECK(provider.calls[0].directory == "/destination/");
    CHECK(provider.calls[0].host == &native_host);
    CHECK(provider.calls[1].directory == "/");
    CHECK(provider.calls[1].host == &remote_host);
    CHECK(provider.calls[2].directory == "/");
    CHECK(provider.calls[2].host == &remote_host);
    CHECK(provider.request_access_calls == 0);
}

TEST_CASE(PREFIX "maps an absent grant and provider exceptions to permission required")
{
    RecordingDirectoryAccessProvider provider;
    const auto checker = MakeVFSOperationPlanningAccessChecker(provider);
    AccessTestHost host{"planning_access_fail_closed"};

    provider.grants_access = false;
    CHECK(StateOf(checker,
                  {"provider", "/directory"},
                  OperationPlanningRequiredAccess::Write,
                  host) == OperationPlanningAccessState::PermissionRequired);

    provider.throw_from_has_access = true;
    CHECK(StateOf(checker,
                  {"provider", "/directory/item"},
                  OperationPlanningRequiredAccess::Read,
                  host) == OperationPlanningAccessState::PermissionRequired);

    CHECK(provider.calls.size() == 2);
    CHECK(provider.request_access_calls == 0);
}

TEST_CASE(PREFIX "fails closed before the provider for malformed path provider or access")
{
    RecordingDirectoryAccessProvider provider;
    const auto checker = MakeVFSOperationPlanningAccessChecker(provider);
    AccessTestHost host{"planning_access_invalid"};
    std::string nul_provider{"provider\0suffix", 15};
    std::string nul_path{"/directory\0item", 15};

    const std::array invalid_paths{
        OperationPlanningPath{"", "/directory"},
        OperationPlanningPath{std::move(nul_provider), "/directory"},
        OperationPlanningPath{"provider", ""},
        OperationPlanningPath{"provider", "relative"},
        OperationPlanningPath{"provider", std::move(nul_path)},
    };
    for( const auto &path : invalid_paths )
        CHECK(StateOf(checker, path, OperationPlanningRequiredAccess::Read, host) ==
              OperationPlanningAccessState::PermissionRequired);
    CHECK(StateOf(checker,
                  {"provider", "/directory"},
                  static_cast<OperationPlanningRequiredAccess>(255),
                  host) == OperationPlanningAccessState::PermissionRequired);

    CHECK(provider.calls.empty());
    CHECK(provider.request_access_calls == 0);
}
