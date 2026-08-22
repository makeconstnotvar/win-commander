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
    InvalidDecision,
    UnsupportedPlanType
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

    /** Builds an authority carrying the seal that proves which review it came from. */
    [[nodiscard]] static nc::vfs::ProviderConditionalCopyReviewedAuthority
    MakeAuthority(std::shared_ptr<ReviewedVFSOperationPreflight> _seal,
                  nc::vfs::ProviderConditionalCopyReviewedClaims _claims);
    /** The Move counterpart. A distinct authority type, built the same way from the same seal. */
    [[nodiscard]] static nc::vfs::ProviderConditionalMoveReviewedAuthority
    MakeMoveAuthority(std::shared_ptr<ReviewedVFSOperationPreflight> _seal,
                      nc::vfs::ProviderConditionalMoveReviewedClaims _claims);
    /** The Delete counterpart. A third distinct authority type, built the same way from the same seal. */
    [[nodiscard]] static nc::vfs::ProviderConditionalDeleteReviewedAuthority
    MakeDeleteAuthority(std::shared_ptr<ReviewedVFSOperationPreflight> _seal,
                        nc::vfs::ProviderConditionalDeleteReviewedClaims _claims);

    VFSBoundOperationPreflight m_Preflight;

    friend class ReviewedOperationFactory;
    friend class SealedReviewedPreflight;
};

/**
 * One review, sealed so a whole plan's worth of authorities can be issued from it.
 *
 * A review is a statement that a person looked at *this plan* and accepted it, and a plan covers
 * every item its report accepted. So one review yields one authority per accepted item - no fewer,
 * and emphatically no more.
 *
 * Both halves of that are enforced, and the second is the security-relevant one:
 *
 * - **An index outside the accepted report is refused.** Nobody reviewed it, so there is nothing to
 *   authorise, and an authority minted for it would claim a review that never happened.
 * - **An index is refused the second time.** Otherwise a caller could ask twice for one reviewed
 *   item and obtain an extra authority to spend on something else entirely - which is exactly the
 *   hole the previous one-shot rule existed to close, and it must not reopen just because a plan may
 *   now carry more than one item.
 *
 * Every authority from one review shares a single seal. That is what makes them provably the product
 * of the same review rather than of several - and it is why the seal is created once here rather
 * than per issue.
 */
class SealedReviewedPreflight final
{
public:
    SealedReviewedPreflight() = delete;
    SealedReviewedPreflight(const SealedReviewedPreflight &) = delete;
    SealedReviewedPreflight &operator=(const SealedReviewedPreflight &) = delete;
    SealedReviewedPreflight(SealedReviewedPreflight &&) noexcept = default;
    SealedReviewedPreflight &operator=(SealedReviewedPreflight &&) noexcept = default;

    [[nodiscard]] static SealedReviewedPreflight Seal(ReviewedVFSOperationPreflight _review);

    [[nodiscard]] const VFSOperationPlanningBindings::Ptr &Bindings() const noexcept;
    [[nodiscard]] const AcceptedOperationPlan &AcceptedPlan() const noexcept;
    /** How many authorities this review can ever yield. */
    [[nodiscard]] size_t AcceptedItemCount() const noexcept;

    [[nodiscard]] std::optional<nc::vfs::ProviderConditionalCopyReviewedAuthority>
    IssueAuthorityForItem(size_t _item_index, nc::vfs::ProviderConditionalCopyReviewedClaims _claims);
    /**
     * The Move counterpart of `IssueAuthorityForItem`, same one-shot-per-index rule. A sealed review
     * covers a plan of one type, never both, so the two issuers share `m_Issued` without risk of one
     * authorising what the other already did.
     */
    [[nodiscard]] std::optional<nc::vfs::ProviderConditionalMoveReviewedAuthority>
    IssueMoveAuthorityForItem(size_t _item_index, nc::vfs::ProviderConditionalMoveReviewedClaims _claims);
    /**
     * The Delete counterpart, same one-shot-per-index rule over the same `m_Issued` bookkeeping - a
     * sealed review covers a plan of one type, never more than one of the three at once.
     */
    [[nodiscard]] std::optional<nc::vfs::ProviderConditionalDeleteReviewedAuthority>
    IssueDeleteAuthorityForItem(size_t _item_index, nc::vfs::ProviderConditionalDeleteReviewedClaims _claims);

private:
    SealedReviewedPreflight(std::shared_ptr<ReviewedVFSOperationPreflight> _review, size_t _item_count);

    std::shared_ptr<ReviewedVFSOperationPreflight> m_Review;
    std::vector<bool> m_Issued;
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
