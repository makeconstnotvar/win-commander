// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationPlanner.h"

#include <VFS/ProviderCapabilities.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::vfs {
class Host;
}

namespace nc::ops {

class ReviewedOperationFactory;

struct VFSOperationPlanningProviderBinding final {
    std::string provider_id;
    std::shared_ptr<nc::vfs::Host> host;
};

enum class VFSOperationPlanningProbesValidationError : uint8_t {
    MissingBindings,
    EmptyProviderId,
    MissingHost,
    DuplicateProviderId,
    DuplicateHost,
    HostNamespaceUnavailable
};

/** Immutable provider namespace shared by preflight and the future operation factory. */
class VFSOperationPlanningBindings final
{
public:
    using Ptr = std::shared_ptr<const VFSOperationPlanningBindings>;

    VFSOperationPlanningBindings() = delete;
    VFSOperationPlanningBindings(const VFSOperationPlanningBindings &) = delete;
    VFSOperationPlanningBindings(VFSOperationPlanningBindings &&) = delete;
    VFSOperationPlanningBindings &operator=(const VFSOperationPlanningBindings &) = delete;
    VFSOperationPlanningBindings &operator=(VFSOperationPlanningBindings &&) = delete;

    [[nodiscard]] static std::expected<Ptr, VFSOperationPlanningProbesValidationError>
    Create(std::vector<VFSOperationPlanningProviderBinding> _bindings);

    [[nodiscard]] std::shared_ptr<nc::vfs::Host> Resolve(std::string_view _provider_id) const noexcept;

private:
    using Providers = std::map<std::string, std::shared_ptr<nc::vfs::Host>, std::less<>>;

    explicit VFSOperationPlanningBindings(Providers _providers);

    Providers m_Providers;
};

class VFSBoundOperationPreflight final
{
public:
    VFSBoundOperationPreflight() = delete;
    VFSBoundOperationPreflight(const VFSBoundOperationPreflight &) = delete;
    VFSBoundOperationPreflight(VFSBoundOperationPreflight &&) noexcept = default;
    VFSBoundOperationPreflight &operator=(const VFSBoundOperationPreflight &) = delete;
    VFSBoundOperationPreflight &operator=(VFSBoundOperationPreflight &&) noexcept = default;

    [[nodiscard]] const VFSOperationPlanningBindings::Ptr &Bindings() const noexcept { return m_Bindings; }
    [[nodiscard]] const OperationPreflightResult &Result() const noexcept { return m_Result; }

private:
    VFSBoundOperationPreflight(VFSOperationPlanningBindings::Ptr _bindings,
                               OperationPreflightResult _result);

    VFSOperationPlanningBindings::Ptr m_Bindings;
    OperationPreflightResult m_Result;

    friend class VFSOperationPlanningProbes;
};

enum class VFSOperationPreflightReviewDecision : uint8_t {
    Approved,
    ApprovedWithDestructiveConfirmation
};

enum class VFSOperationPreflightReviewError : uint8_t {
    Blocked,
    DestructiveConfirmationRequired,
    InvalidDecision
};

/** Explicit review token required before a bound accepted preflight can reach an operation factory. */
class ReviewedVFSOperationPreflight final
{
public:
    ReviewedVFSOperationPreflight() = delete;
    ReviewedVFSOperationPreflight(const ReviewedVFSOperationPreflight &) = delete;
    ReviewedVFSOperationPreflight(ReviewedVFSOperationPreflight &&_other) noexcept;
    ReviewedVFSOperationPreflight &operator=(const ReviewedVFSOperationPreflight &) = delete;
    ReviewedVFSOperationPreflight &operator=(ReviewedVFSOperationPreflight &&_other) noexcept;

    [[nodiscard]] static std::expected<ReviewedVFSOperationPreflight, VFSOperationPreflightReviewError>
    Review(VFSBoundOperationPreflight _preflight, VFSOperationPreflightReviewDecision _decision);

    [[nodiscard]] const VFSOperationPlanningBindings::Ptr &Bindings() const noexcept
    {
        return m_Preflight.Bindings();
    }
    [[nodiscard]] const AcceptedOperationPlan &AcceptedPlan() const noexcept;

private:
    explicit ReviewedVFSOperationPreflight(VFSBoundOperationPreflight _preflight);

    [[nodiscard]] std::optional<nc::vfs::ProviderConditionalCopyReviewedAuthority>
    ConsumeConditionalCopyAuthority(nc::vfs::ProviderConditionalCopyReviewedClaims _claims) &&;

    VFSBoundOperationPreflight m_Preflight;
    bool m_AuthorityConsumed{false};

    friend class ReviewedOperationFactory;
};

/**
 * Synchronous production adapter from explicitly bound VFS hosts to the pure planning probe boundary.
 * Its immutable bindings own every host for the duration of preflight and reject duplicate semantic
 * namespaces, so distinct provider ids cannot bypass same-path or recursive-destination checks.
 */
class VFSOperationPlanningProbes final : public OperationPlanningProbes
{
public:
    using CancelChecker = std::function<bool()>;
    using AccessChecker = std::function<OperationPlanningProbeResult<OperationPlanningAccessEvidence>(
        const OperationPlanningPath &,
        OperationPlanningRequiredAccess,
        nc::vfs::Host &)>;

    VFSOperationPlanningProbes() = delete;
    VFSOperationPlanningProbes(const VFSOperationPlanningProbes &) = delete;
    VFSOperationPlanningProbes(VFSOperationPlanningProbes &&) noexcept = default;
    VFSOperationPlanningProbes &operator=(const VFSOperationPlanningProbes &) = delete;
    VFSOperationPlanningProbes &operator=(VFSOperationPlanningProbes &&) noexcept = default;

    [[nodiscard]] static std::expected<VFSOperationPlanningProbes, VFSOperationPlanningProbesValidationError>
    Create(VFSOperationPlanningBindings::Ptr _bindings,
           AccessChecker _access_checker = {},
           CancelChecker _cancel_checker = {});

    /** Runs preflight and retains the exact provider bindings required by the future operation factory. */
    [[nodiscard]] VFSBoundOperationPreflight Preflight(OperationPlan _plan);

    OperationPlanningProbeResult<OperationPlanningProviderEvidence>
    ProbeProvider(const OperationPlanningPath &_path) override;
    OperationPlanningProbeResult<OperationPlanningItemEvidence>
    ProbeItem(const OperationPlanningPath &_path) override;
    OperationPlanningProbeResult<OperationPlanningNameEvidence>
    ProbeDestinationName(const OperationPlanningPath &_path) override;
    OperationPlanningProbeResult<OperationPlanningAccessEvidence>
    ProbeAccess(const OperationPlanningPath &_path, OperationPlanningRequiredAccess _required) override;
    OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
    ProbeEstimate(const OperationPlanningPath &_source,
                  const OperationPlanningPath &_destination) override;
    OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
    ProbeSpace(const OperationPlanningPath &_destination_directory) override;

private:
    VFSOperationPlanningProbes(VFSOperationPlanningBindings::Ptr _bindings,
                               AccessChecker _access_checker,
                               CancelChecker _cancel_checker);

    OperationPlanningProbeResult<OperationPlanningProviderEvidence>
    ProbeProviderImpl(const OperationPlanningPath &_path);
    OperationPlanningProbeResult<OperationPlanningItemEvidence>
    ProbeItemImpl(const OperationPlanningPath &_path);
    OperationPlanningProbeResult<OperationPlanningNameEvidence>
    ProbeDestinationNameImpl(const OperationPlanningPath &_path);
    OperationPlanningProbeResult<OperationPlanningAccessEvidence>
    ProbeAccessImpl(const OperationPlanningPath &_path, OperationPlanningRequiredAccess _required);
    OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
    ProbeEstimateImpl(const OperationPlanningPath &_source,
                      const OperationPlanningPath &_destination);
    OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
    ProbeSpaceImpl(const OperationPlanningPath &_destination_directory);

    [[nodiscard]] std::shared_ptr<nc::vfs::Host> FindHost(std::string_view _provider_id) const noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;
    [[nodiscard]] std::function<bool()> SanitizedCancelChecker() const;

    VFSOperationPlanningBindings::Ptr m_Bindings;
    AccessChecker m_AccessChecker;
    CancelChecker m_CancelChecker;
};

} // namespace nc::ops
