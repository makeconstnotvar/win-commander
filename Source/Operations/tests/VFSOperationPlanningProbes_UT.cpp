// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include <Operations/OperationPlan.h>
#include <Operations/OperationPlanner.h>
#include <Operations/VFSOperationPlanningProbes.h>

#include <Base/Error.h>
#include <VFS/Host.h>

#include <cerrno>
#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "VFSOperationPlanningProbes: "

namespace nc::ops {
namespace {

std::string NextPlanningNamespace()
{
    static uint64_t next = 0;
    return "planning-namespace-" + std::to_string(++next);
}

struct PlanningHostConfiguration final {
    std::string namespace_id;

    [[nodiscard]] static const char *Tag() noexcept { return "planning-test"; }
    [[nodiscard]] const char *Junction() const noexcept { return namespace_id.c_str(); }
    bool operator==(const PlanningHostConfiguration &) const = default;
};

class PlanningHost final : public nc::vfs::Host
{
public:
    PlanningHost(uint64_t _features,
                 bool _writable,
                 bool _case_sensitive,
                 bool _native,
                 std::string _namespace_id = NextPlanningNamespace())
        : Host("", nullptr, "planning-test"), m_Writable(_writable), m_CaseSensitive(_case_sensitive),
          m_Native(_native), m_Configuration(PlanningHostConfiguration{std::move(_namespace_id)})
    {
        AddFeatures(_features);
    }

    VFSConfiguration Configuration() const override { return m_Configuration; }
    bool IsWritable() const override { return m_Writable; }
    bool IsWritableAtPath(std::string_view) const override
    {
        if( after_capability_query )
            after_capability_query();
        return m_Writable;
    }
    bool IsCaseSensitiveAtPath(std::string_view) const override { return m_CaseSensitive; }
    std::optional<bool> CaseSensitivityAtPath(std::string_view) const override
    {
        return m_Native ? std::optional<bool>{m_CaseSensitive} : std::nullopt;
    }
    std::optional<std::string> SemanticNamespaceIdentity() const override
    {
        return m_Configuration.Get<PlanningHostConfiguration>().namespace_id;
    }
    bool IsNativeFS() const noexcept override { return m_Native; }
    bool ValidateFilename(std::string_view _filename) const override
    {
        if( after_name_validation )
            after_name_validation();
        return !invalid_names.contains(_filename) && Host::ValidateFilename(_filename);
    }

    std::expected<VFSStat, Error>
    Stat(std::string_view _path, unsigned long, const VFSCancelChecker &_cancel_checker) override
    {
        if( m_Iterating )
            stat_called_during_iteration = true;
        if( _cancel_checker && _cancel_checker() )
            return std::unexpected(Error{Error::POSIX, ECANCELED});
        if( after_stat )
            after_stat();
        if( const auto error = stat_errors.find(_path); error != stat_errors.end() )
            return std::unexpected(error->second);
        if( const auto stat = stats.find(_path); stat != stats.end() )
            return stat->second;
        return std::unexpected(Error{Error::POSIX, ENOENT});
    }

    std::expected<VFSStatFS, Error>
    StatFS(std::string_view, const VFSCancelChecker &_cancel_checker) override
    {
        if( _cancel_checker && _cancel_checker() )
            return std::unexpected(Error{Error::POSIX, ECANCELED});
        if( after_statfs )
            after_statfs();
        if( statfs_error )
            return std::unexpected(*statfs_error);
        return statfs;
    }

    std::expected<void, Error>
    IterateDirectoryListing(std::string_view _path,
                            const std::function<bool(const VFSDirEnt &)> &_handler) override
    {
        if( const auto error = iteration_errors.find(_path); error != iteration_errors.end() )
            return std::unexpected(error->second);
        const auto listing = listings.find(_path);
        if( listing == listings.end() )
            return std::unexpected(Error{Error::POSIX, ENOENT});
        m_Iterating = true;
        for( const std::string &name : listing->second ) {
            const VFSDirEnt entry{.type = VFSDirEnt::Unknown, .name = name};
            if( !_handler(entry) )
                break;
        }
        m_Iterating = false;
        if( iteration_error_after_entries )
            return std::unexpected(*iteration_error_after_entries);
        return {};
    }

    std::map<std::string, VFSStat, std::less<>> stats;
    std::map<std::string, Error, std::less<>> stat_errors;
    std::map<std::string, std::vector<std::string>, std::less<>> listings;
    std::map<std::string, Error, std::less<>> iteration_errors;
    VFSStatFS statfs;
    std::optional<Error> statfs_error;
    std::optional<Error> iteration_error_after_entries;
    std::function<void()> after_stat;
    std::function<void()> after_statfs;
    mutable std::function<void()> after_capability_query;
    mutable std::function<void()> after_name_validation;
    std::set<std::string, std::less<>> invalid_names;
    bool stat_called_during_iteration = false;

private:
    bool m_Writable;
    bool m_CaseSensitive;
    bool m_Native;
    VFSConfiguration m_Configuration;
    bool m_Iterating = false;
};

VFSStat Stat(mode_t _mode, uint64_t _size = 0, bool _size_known = true)
{
    VFSStat stat;
    stat.mode = static_cast<uint16_t>(_mode);
    stat.size = _size;
    stat.meaning.mode = 1;
    stat.meaning.size = _size_known;
    return stat;
}

std::shared_ptr<PlanningHost>
ReadHost(bool _case_sensitive = true, bool _native = false, std::string _namespace_id = NextPlanningNamespace())
{
    return std::make_shared<PlanningHost>(
        nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::ReadSymlink,
        false,
        _case_sensitive,
        _native,
        std::move(_namespace_id));
}

std::shared_ptr<PlanningHost>
WriteHost(bool _case_sensitive = true, bool _native = false, std::string _namespace_id = NextPlanningNamespace())
{
    return std::make_shared<PlanningHost>(
        nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::CreateFile |
            nc::vfs::HostFeatures::CreateDirectory | nc::vfs::HostFeatures::Unlink |
            nc::vfs::HostFeatures::RemoveDirectory | nc::vfs::HostFeatures::CreateSymlink,
        true,
        _case_sensitive,
        _native,
        std::move(_namespace_id));
}

VFSOperationPlanningProbes MakeProbes(std::vector<VFSOperationPlanningProviderBinding> _bindings,
                                      VFSOperationPlanningProbes::AccessChecker _access_checker = {},
                                      VFSOperationPlanningProbes::CancelChecker _cancel_checker = {})
{
    auto bindings = VFSOperationPlanningBindings::Create(std::move(_bindings));
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings, std::move(_access_checker), std::move(_cancel_checker));
    REQUIRE(probes);
    return std::move(*probes);
}

OperationPlan CopyDirectoryPlan()
{
    OperationPlanInput input{
        .plan_id = "copy-directory",
        .type = OperationPlanType::Copy,
        .sources = {{"source-instance", "/src/tree"}},
        .destination = OperationPlanDestinationInput{
            "destination-instance", "/dst", OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{
            OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

} // namespace

TEST_CASE(PREFIX "validates explicit owning provider bindings", "[vfs-operation-planning-probes]")
{
    const auto host = ReadHost();

    CHECK(VFSOperationPlanningBindings::Create({{"source", host}}));
    CHECK(VFSOperationPlanningBindings::Create({{"", host}}).error() ==
          VFSOperationPlanningProbesValidationError::EmptyProviderId);
    CHECK(VFSOperationPlanningBindings::Create({{"source", nullptr}}).error() ==
          VFSOperationPlanningProbesValidationError::MissingHost);
    CHECK(VFSOperationPlanningBindings::Create({{"source", host}, {"source", host}}).error() ==
          VFSOperationPlanningProbesValidationError::DuplicateProviderId);
    CHECK(VFSOperationPlanningBindings::Create({{"source", host}, {"alias", host}}).error() ==
          VFSOperationPlanningProbesValidationError::DuplicateHost);

    const auto semantic_alias_a = ReadHost(true, false, "same-namespace");
    const auto semantic_alias_b = ReadHost(false, false, "same-namespace");
    CHECK(VFSOperationPlanningBindings::Create(
              {{"first", semantic_alias_a}, {"second", semantic_alias_b}}).error() ==
          VFSOperationPlanningProbesValidationError::DuplicateHost);

    const std::shared_ptr<nc::vfs::Host> second_control_block(host.get(), [](nc::vfs::Host *) {});
    CHECK(VFSOperationPlanningBindings::Create(
              {{"source", host}, {"alias", second_control_block}}).error() ==
          VFSOperationPlanningProbesValidationError::DuplicateHost);

    const auto unknown_a = std::make_shared<nc::vfs::Host>("", nullptr, "unknown-planning-host");
    const auto unknown_b = std::make_shared<nc::vfs::Host>("", nullptr, "unknown-planning-host");
    CHECK(VFSOperationPlanningBindings::Create({{"first", unknown_a}, {"second", unknown_b}}).error() ==
          VFSOperationPlanningProbesValidationError::HostNamespaceUnavailable);
    CHECK(VFSOperationPlanningProbes::Create(nullptr).error() ==
          VFSOperationPlanningProbesValidationError::MissingBindings);

    auto owning_host = ReadHost();
    std::weak_ptr<PlanningHost> weak = owning_host;
    auto owned = MakeProbes({{"source", owning_host}});
    owning_host.reset();
    CHECK_FALSE(weak.expired());
    CHECK(owned.ProbeProvider({"source", "/"}));

    auto shared_bindings = VFSOperationPlanningBindings::Create({{"source", host}});
    REQUIRE(shared_bindings);
    CHECK((*shared_bindings)->Resolve("source") == host);
    CHECK_FALSE((*shared_bindings)->Resolve("missing"));
}

TEST_CASE(PREFIX "projects VFS capabilities and path identity conservatively",
          "[vfs-operation-planning-probes]")
{
    const auto source = ReadHost(true, true);
    source->stats.emplace("/src", Stat(S_IFDIR | 0755));
    const auto destination = WriteHost(false, true);
    destination->stats.emplace("/dst", Stat(S_IFDIR | 0755));
    const auto file_only_destination = std::make_shared<PlanningHost>(
        nc::vfs::HostFeatures::CreateFile, true, true, false);
    auto probes = MakeProbes({
        {"source", source},
        {"destination", destination},
        {"file-only", file_only_destination},
    });

    const auto source_evidence = probes.ProbeProvider({"source", "/src"});
    REQUIRE(source_evidence);
    CHECK(source_evidence->can_copy_from);
    CHECK_FALSE(source_evidence->can_copy_to);
    CHECK(source_evidence->path_identity == OperationPlanningPathIdentitySemantics::ASCIICaseSensitive);

    const auto destination_evidence = probes.ProbeProvider({"destination", "/dst"});
    REQUIRE(destination_evidence);
    CHECK(destination_evidence->can_copy_from);
    CHECK(destination_evidence->can_copy_to);
    CHECK(destination_evidence->path_identity ==
          OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive);

    const auto file_only_evidence = probes.ProbeProvider({"file-only", "/dst"});
    REQUIRE(file_only_evidence);
    CHECK_FALSE(file_only_evidence->can_copy_to);
    CHECK(file_only_evidence->path_identity == OperationPlanningPathIdentitySemantics::Unavailable);

    const auto unknown = probes.ProbeProvider({"unbound", "/"});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error() == OperationPlanningProbeError::Unavailable);
}

TEST_CASE(PREFIX "maps item, access, cancellation, and space evidence", "[vfs-operation-planning-probes]")
{
    const auto source = ReadHost();
    source->stats.emplace("/file", Stat(S_IFREG | 0644, 42));
    source->stats.emplace("/dir", Stat(S_IFDIR | 0755));
    source->stats.emplace("/symlink", Stat(S_IFLNK | 0777));
    source->stats.emplace("/fifo", Stat(S_IFIFO | 0600));
    source->stats.emplace("/unknown-mode", VFSStat{});
    source->stat_errors.emplace("/denied", Error{Error::POSIX, EACCES});
    source->statfs.avail_bytes = 1234;
    auto probes = MakeProbes({{"source", source}});

    CHECK(probes.ProbeItem({"source", "/file"}) ==
          OperationPlanningItemEvidence{OperationPlanningItemKind::File, 42});
    CHECK(probes.ProbeItem({"source", "/dir"}) ==
          OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    CHECK(probes.ProbeItem({"source", "/missing"}) ==
          OperationPlanningItemEvidence{OperationPlanningItemKind::Missing, std::nullopt});
    const auto unknown_mode = probes.ProbeItem({"source", "/unknown-mode"});
    REQUIRE_FALSE(unknown_mode);
    CHECK(unknown_mode.error() == OperationPlanningProbeError::UnsupportedItem);
    CHECK(probes.ProbeItem({"source", "/symlink"}) ==
          OperationPlanningItemEvidence{OperationPlanningItemKind::Symlink, 0});
    const auto fifo = probes.ProbeItem({"source", "/fifo"});
    REQUIRE_FALSE(fifo);
    CHECK(fifo.error() == OperationPlanningProbeError::UnsupportedItem);

    source->invalid_names.emplace("forbidden:name");
    CHECK(probes.ProbeDestinationName({"source", "/folder/valid.txt"}) ==
          OperationPlanningNameEvidence{true});
    CHECK(probes.ProbeDestinationName({"source", "/folder/forbidden:name"}) ==
          OperationPlanningNameEvidence{false});
    CHECK(probes.ProbeDestinationName({"source", "/"}) == OperationPlanningNameEvidence{false});

    const auto denied = probes.ProbeItem({"source", "/denied"});
    REQUIRE_FALSE(denied);
    CHECK(denied.error() == OperationPlanningProbeError::PermissionDenied);
    CHECK(probes.ProbeAccess({"source", "/file"}, OperationPlanningRequiredAccess::Read) ==
          OperationPlanningAccessEvidence{OperationPlanningAccessState::PermissionRequired});
    CHECK(probes.ProbeAccess({"source", "/file"}, OperationPlanningRequiredAccess::Write) ==
          OperationPlanningAccessEvidence{OperationPlanningAccessState::Denied});
    source->statfs.total_bytes = 2000;
    source->statfs.free_bytes = 1500;
    CHECK(probes.ProbeSpace({"source", "/"}) == OperationPlanningSpaceEvidence{1234});

    source->statfs.avail_bytes = 0;
    CHECK(probes.ProbeSpace({"source", "/"}) == OperationPlanningSpaceEvidence{0});

    source->statfs = {};
    CHECK(probes.ProbeSpace({"source", "/"}) == OperationPlanningSpaceEvidence{std::nullopt});

    auto cancelled = MakeProbes({{"source", source}}, {}, [] { return true; });
    const auto cancelled_item = cancelled.ProbeItem({"source", "/file"});
    REQUIRE_FALSE(cancelled_item);
    CHECK(cancelled_item.error() == OperationPlanningProbeError::Cancelled);

    bool cancel_after_stat = false;
    source->after_stat = [&] { cancel_after_stat = true; };
    auto late_stat_cancel = MakeProbes({{"source", source}}, {}, [&] { return cancel_after_stat; });
    const auto cancelled_after_stat = late_stat_cancel.ProbeItem({"source", "/file"});
    REQUIRE_FALSE(cancelled_after_stat);
    CHECK(cancelled_after_stat.error() == OperationPlanningProbeError::Cancelled);
    source->after_stat = {};

    auto throwing_cancel = MakeProbes({{"source", source}}, {}, []() -> bool { throw 1; });
    const auto contained = throwing_cancel.ProbeSpace({"source", "/"});
    REQUIRE_FALSE(contained);
    CHECK(contained.error() == OperationPlanningProbeError::Cancelled);

    int cancel_checks = 0;
    auto late_throwing_cancel = MakeProbes({{"source", source}}, {}, [&] {
        if( ++cancel_checks == 1 )
            return false;
        throw 1;
    });
    const auto contained_late_throw = late_throwing_cancel.ProbeItem({"source", "/file"});
    REQUIRE_FALSE(contained_late_throw);
    CHECK(contained_late_throw.error() == OperationPlanningProbeError::Cancelled);

    const auto native = ReadHost(true, true);
    native->stats.emplace("/file", Stat(S_IFREG | 0644, 1));
    auto native_without_access_checker = MakeProbes({{"native", native}});
    CHECK(native_without_access_checker.ProbeAccess(
              {"native", "/file"}, OperationPlanningRequiredAccess::Read) ==
          OperationPlanningAccessEvidence{OperationPlanningAccessState::PermissionRequired});

    auto native_with_access_checker = MakeProbes(
        {{"native", native}},
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    CHECK(native_with_access_checker.ProbeAccess(
              {"native", "/file"}, OperationPlanningRequiredAccess::Read) ==
          OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted});

    auto throwing_access_checker = MakeProbes(
        {{"native", native}},
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> { throw 1; });
    const auto contained_access = throwing_access_checker.ProbeAccess(
        {"native", "/file"}, OperationPlanningRequiredAccess::Read);
    REQUIRE_FALSE(contained_access);
    CHECK(contained_access.error() == OperationPlanningProbeError::Failed);

    bool cancel_after_statfs = false;
    source->after_statfs = [&] { cancel_after_statfs = true; };
    auto late_space_cancel = MakeProbes({{"source", source}}, {}, [&] { return cancel_after_statfs; });
    const auto cancelled_space = late_space_cancel.ProbeSpace({"source", "/"});
    REQUIRE_FALSE(cancelled_space);
    CHECK(cancelled_space.error() == OperationPlanningProbeError::Cancelled);
    source->after_statfs = {};

    bool cancel_after_access = false;
    auto late_access_cancel = MakeProbes(
        {{"native", native}},
        [&](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            cancel_after_access = true;
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        },
        [&] { return cancel_after_access; });
    const auto cancelled_access = late_access_cancel.ProbeAccess(
        {"native", "/file"}, OperationPlanningRequiredAccess::Read);
    REQUIRE_FALSE(cancelled_access);
    CHECK(cancelled_access.error() == OperationPlanningProbeError::Cancelled);

    bool cancel_during_capability_query = false;
    native->after_capability_query = [&] { cancel_during_capability_query = true; };
    auto capability_cancel = MakeProbes(
        {{"native", native}}, {}, [&] { return cancel_during_capability_query; });
    const auto cancelled_capability = capability_cancel.ProbeAccess(
        {"native", "/file"}, OperationPlanningRequiredAccess::Read);
    REQUIRE_FALSE(cancelled_capability);
    CHECK(cancelled_capability.error() == OperationPlanningProbeError::Cancelled);
    native->after_capability_query = {};

    const auto remote = ReadHost();
    remote->stats.emplace("/dir", Stat(S_IFDIR | 0755));
    auto remote_probes = MakeProbes({{"remote", remote}});
    const auto remote_estimate = remote_probes.ProbeEstimate({"remote", "/dir"}, {"remote", "/target"});
    REQUIRE_FALSE(remote_estimate);
    CHECK(remote_estimate.error() == OperationPlanningProbeError::Unsupported);
}

TEST_CASE(PREFIX "emits complete native object identity and version evidence",
          "[vfs-operation-planning-probes]")
{
    SECTION("real native host")
    {
        const TempTestDir directory;
        const auto path = directory.directory / "identity.txt";
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(write(fd, "identity", 8) == 8);
        REQUIRE(close(fd) == 0);

        const auto host = TestEnv().vfs_native;
        auto probes = MakeProbes({{"native", host}});
        const auto evidence = probes.ProbeItem({"native", path.native()});
        REQUIRE(evidence);
        REQUIRE(evidence->native_identity);
        REQUIRE(evidence->native_version);

        const auto stat = host->Stat(path.native(), VFSFlags::F_NoFollow);
        REQUIRE(stat);
        REQUIRE(stat->meaning.dev);
        REQUIRE(stat->meaning.inode);
        REQUIRE(stat->meaning.btime);
        REQUIRE(stat->meaning.mode);
        REQUIRE(stat->meaning.size);
        REQUIRE(stat->meaning.mtime);
        REQUIRE(stat->meaning.ctime);
        CHECK(*evidence->native_identity == OperationPlanningNativeObjectIdentityEvidence{
                                                .device = stat->dev,
                                                .inode = stat->inode,
                                                .birth_time = {
                                                    .seconds = static_cast<int64_t>(stat->btime.tv_sec),
                                                    .nanoseconds = static_cast<int64_t>(stat->btime.tv_nsec),
                                                },
                                            });
        CHECK(*evidence->native_version == OperationPlanningNativeObjectVersionEvidence{
                                               .mode = stat->mode,
                                               .byte_size = stat->size,
                                               .modification_time = {
                                                   .seconds = static_cast<int64_t>(stat->mtime.tv_sec),
                                                   .nanoseconds = static_cast<int64_t>(stat->mtime.tv_nsec),
                                               },
                                               .status_change_time = {
                                                   .seconds = static_cast<int64_t>(stat->ctime.tv_sec),
                                                   .nanoseconds = static_cast<int64_t>(stat->ctime.tv_nsec),
                                               },
                                           });
    }

    SECTION("non-native and incomplete native stats do not invent tokens")
    {
        auto complete_stat = Stat(S_IFREG | 0644, 9);
        complete_stat.dev = 3;
        complete_stat.inode = 5;
        complete_stat.btime = {.tv_sec = 7, .tv_nsec = 11};
        complete_stat.mtime = {.tv_sec = 13, .tv_nsec = 17};
        complete_stat.ctime = {.tv_sec = 19, .tv_nsec = 23};
        complete_stat.meaning.dev = 1;
        complete_stat.meaning.inode = 1;
        complete_stat.meaning.btime = 1;
        complete_stat.meaning.mtime = 1;
        complete_stat.meaning.ctime = 1;

        const auto remote = ReadHost();
        remote->stats.emplace("/file", complete_stat);
        auto remote_probes = MakeProbes({{"remote", remote}});
        const auto remote_evidence = remote_probes.ProbeItem({"remote", "/file"});
        REQUIRE(remote_evidence);
        CHECK_FALSE(remote_evidence->native_identity);
        CHECK_FALSE(remote_evidence->native_version);

        complete_stat.meaning.btime = 0;
        complete_stat.meaning.ctime = 0;
        const auto incomplete_native = ReadHost(true, true);
        incomplete_native->stats.emplace("/file", complete_stat);
        auto native_probes = MakeProbes({{"native", incomplete_native}});
        const auto native_evidence = native_probes.ProbeItem({"native", "/file"});
        REQUIRE(native_evidence);
        CHECK_FALSE(native_evidence->native_identity);
        CHECK_FALSE(native_evidence->native_version);

        const auto missing_evidence = native_probes.ProbeItem({"native", "/missing"});
        REQUIRE(missing_evidence);
        CHECK(missing_evidence->kind == OperationPlanningItemKind::Missing);
        CHECK_FALSE(missing_evidence->native_identity);
        CHECK_FALSE(missing_evidence->native_version);
    }
}

TEST_CASE(PREFIX "drives copy preflight with recursive estimates and free space",
          "[vfs-operation-planning-probes]")
{
    const auto source = ReadHost(true, true);
    source->stats.emplace("/src/tree", Stat(S_IFDIR | 0755));
    source->stats.emplace("/src/tree/a", Stat(S_IFREG | 0644, 5));
    source->stats.emplace("/src/tree/sub", Stat(S_IFDIR | 0755));
    source->stats.emplace("/src/tree/sub/b", Stat(S_IFREG | 0644, 7));
    source->listings.emplace("/src/tree", std::vector<std::string>{"a", "sub"});
    source->listings.emplace("/src/tree/sub", std::vector<std::string>{"b"});

    const auto destination = WriteHost(true, true);
    destination->stats.emplace("/dst", Stat(S_IFDIR | 0755));
    destination->statfs.avail_bytes = 20;

    auto probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto bound_result = probes.Preflight(CopyDirectoryPlan());
    CHECK(bound_result.Bindings()->Resolve("source-instance") == source);
    CHECK(bound_result.Bindings()->Resolve("destination-instance") == destination);
    const auto &result = bound_result.Result();
    REQUIRE(std::holds_alternative<AcceptedOperationPlan>(result));
    const OperationPreflightReport &report = std::get<AcceptedOperationPlan>(result).Report();
    REQUIRE(report.items.size() == 1);
    CHECK(report.items.front().estimate == OperationPlanningEstimateEvidence{2, 12});
    CHECK(report.estimated_files == 2);
    CHECK(report.estimated_bytes == 12);
    CHECK(report.destination_space == OperationPlanningSpaceEvidence{20});
    CHECK_FALSE(report.requires_confirmation);
    CHECK_FALSE(source->stat_called_during_iteration);

    source->stats.emplace("/src/tree/bad:name", Stat(S_IFREG | 0644, 1));
    source->listings["/src/tree"].emplace_back("bad:name");
    auto invalid_nested_name_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto invalid_nested_name_result =
        OperationPlanner::Preflight(CopyDirectoryPlan(), invalid_nested_name_probes);
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(invalid_nested_name_result));
    CHECK(std::ranges::any_of(
        std::get<BlockedOperationPlan>(invalid_nested_name_result).Blockers(),
        [](const OperationPlanningBlocker &_blocker) {
            return _blocker.code == OperationPlanningBlockerCode::InvalidDestinationName;
        }));
    source->listings["/src/tree"].pop_back();
    source->stats.erase("/src/tree/bad:name");

    const auto local = WriteHost(true, true);
    local->invalid_names.emplace("bad:name");
    local->stats.emplace("/src/tree", Stat(S_IFDIR | 0755));
    local->stats.emplace("/src/tree/bad:name", Stat(S_IFREG | 0644, 1));
    local->stats.emplace("/dst", Stat(S_IFDIR | 0755));
    local->listings.emplace("/src/tree", std::vector<std::string>{"bad:name"});
    local->statfs.avail_bytes = 20;
    OperationPlanInput same_provider_input{
        .plan_id = "same-provider-copy-directory",
        .type = OperationPlanType::Copy,
        .sources = {{"local", "/src/tree"}},
        .destination = OperationPlanDestinationInput{
            "local", "/dst", OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{
            OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto same_provider_plan = OperationPlan::Create(std::move(same_provider_input));
    REQUIRE(same_provider_plan);
    auto same_provider_probes = MakeProbes(
        {{"local", local}},
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto same_provider_result =
        OperationPlanner::Preflight(std::move(*same_provider_plan), same_provider_probes);
    REQUIRE(std::holds_alternative<AcceptedOperationPlan>(same_provider_result));
    REQUIRE(std::get<AcceptedOperationPlan>(same_provider_result).Report().items.front().estimate);
    CHECK(std::get<AcceptedOperationPlan>(same_provider_result)
              .Report()
              .items.front()
              .estimate->files == 1);

    source->iteration_error_after_entries = Error{Error::POSIX, EIO};
    auto partial_listing_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto partial_listing_result = OperationPlanner::Preflight(CopyDirectoryPlan(), partial_listing_probes);
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(partial_listing_result));
    CHECK(std::ranges::any_of(
        std::get<BlockedOperationPlan>(partial_listing_result).Blockers(),
        [](const OperationPlanningBlocker &_blocker) {
            return _blocker.code == OperationPlanningBlockerCode::ProbeFailed;
        }));
    source->iteration_error_after_entries.reset();

    source->stat_errors.emplace("/src/tree/a", Error{Error::POSIX, ENOENT});
    auto vanished_child_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto vanished_child_result = OperationPlanner::Preflight(CopyDirectoryPlan(), vanished_child_probes);
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(vanished_child_result));
    const auto &vanished_blockers = std::get<BlockedOperationPlan>(vanished_child_result).Blockers();
    CHECK(std::ranges::any_of(vanished_blockers, [](const OperationPlanningBlocker &_blocker) {
        return _blocker.code == OperationPlanningBlockerCode::ProbeFailed;
    }));
    CHECK_FALSE(std::ranges::any_of(vanished_blockers, [](const OperationPlanningBlocker &_blocker) {
        return _blocker.code == OperationPlanningBlockerCode::ProviderUnavailable;
    }));
    source->stat_errors.erase("/src/tree/a");

    source->stats.emplace("/src/tree/link", Stat(S_IFLNK | 0777, 4));
    source->listings["/src/tree"].emplace_back("link");
    auto symlink_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto symlink_result = OperationPlanner::Preflight(CopyDirectoryPlan(), symlink_probes);
    REQUIRE(std::holds_alternative<AcceptedOperationPlan>(symlink_result));
    REQUIRE(std::get<AcceptedOperationPlan>(symlink_result).Report().items.front().estimate);
    CHECK(std::get<AcceptedOperationPlan>(symlink_result)
              .Report()
              .items.front()
              .estimate->contains_symlinks);
    CHECK(std::get<AcceptedOperationPlan>(symlink_result).Report().estimated_files == 3);
    CHECK(std::get<AcceptedOperationPlan>(symlink_result).Report().estimated_bytes == 16);

    const auto no_symlink_destination = std::make_shared<PlanningHost>(
        nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::CreateFile |
            nc::vfs::HostFeatures::CreateDirectory | nc::vfs::HostFeatures::Unlink |
            nc::vfs::HostFeatures::RemoveDirectory,
        true,
        true,
        true);
    no_symlink_destination->stats.emplace("/dst", Stat(S_IFDIR | 0755));
    no_symlink_destination->statfs.avail_bytes = 20;
    auto no_symlink_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", no_symlink_destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto no_symlink_result = OperationPlanner::Preflight(CopyDirectoryPlan(), no_symlink_probes);
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(no_symlink_result));
    CHECK(std::ranges::any_of(
        std::get<BlockedOperationPlan>(no_symlink_result).Blockers(),
        [](const OperationPlanningBlocker &_blocker) {
            return _blocker.code == OperationPlanningBlockerCode::ProviderCapabilityUnsupported;
        }));

    source->stats.emplace("/src/tree/fifo", Stat(S_IFIFO | 0600));
    source->listings["/src/tree"].emplace_back("fifo");
    auto blocked_probes = MakeProbes(
        {
            {"source-instance", source},
            {"destination-instance", destination},
        },
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    const auto blocked_result = OperationPlanner::Preflight(CopyDirectoryPlan(), blocked_probes);
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(blocked_result));
    const auto &blockers = std::get<BlockedOperationPlan>(blocked_result).Blockers();
    CHECK(std::ranges::any_of(blockers, [](const OperationPlanningBlocker &_blocker) {
        return _blocker.code == OperationPlanningBlockerCode::ProviderCapabilityUnsupported;
    }));
}

} // namespace nc::ops

#undef PREFIX
