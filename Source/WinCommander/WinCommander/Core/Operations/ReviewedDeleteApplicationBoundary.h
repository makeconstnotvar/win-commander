// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Operations/CopyOperationOrchestrator.h>
#include <Operations/VFSOperationPlanningProbes.h>
#include <WinCommander/Core/Operations/OperationSubmissionGate.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nc::panel::actions::reviewed_delete {

/** One accepted item, named by its source alone - a Delete publishes nothing for a second end to name. */
struct ReviewPresentationItem final {
    std::string source_path;

    bool operator==(const ReviewPresentationItem &) const = default;
};

/**
 * App-owned immutable projection of the exact accepted Delete plan shown before an authority is minted.
 * The `reviewed_copy_as` counterpart of this carries a destination root, kind, and conflict policy;
 * none of those exist for a Delete plan at all - `OperationPlan::Create` refuses both a destination and
 * a conflict policy for `PermanentDelete`, the same way it refuses them for `Trash` - so this projection
 * has nothing to carry in their place rather than an empty or inert stand-in for them.
 */
struct ReviewPresentation final {
    std::string plan_id;
    /** Every accepted item, in the plan's source order. Never empty for an accepted scope. */
    std::vector<ReviewPresentationItem> items;
    std::optional<uint64_t> estimated_files;
    std::optional<uint64_t> estimated_bytes;
    size_t provider_evidence_count = 0;
    size_t item_evidence_count = 0;
    size_t access_evidence_count = 0;
    std::vector<nc::ops::OperationPlanningWarningCode> warnings;

    bool operator==(const ReviewPresentation &) const = default;
};

enum class PreparationErrorCode : uint8_t {
    UnpersistedRuntime,
    StaleIntent,
    BlockedPreflight,
    UnsupportedScope
};

struct PreparationError final {
    PreparationErrorCode code;
    std::optional<nc::ops::OperationPlanningBlockerCode> blocker;

    bool operator==(const PreparationError &) const = default;
};

enum class ApprovalResult : uint8_t {
    Submitted,
    Declined,
    StaleIntent,
    Cancelled,
    ReviewFailed,
    AlreadyConsumed,
    MissingSubmissionPort,
    SubmissionPortFailed
};

/**
 * Narrow production seam between the Delete UI glue and the reviewed operation lifecycle - the Delete
 * counterpart of `reviewed_copy_as::PreparedReview`, duplicated rather than shared for the same reason
 * `ProviderConditionalDeleteReviewedAuthority` is its own type: nothing about `Approve` here depends on
 * a destination existing, but a shared base would still tie a Delete's approval path to Copy/Move's own
 * evolution, and the two currently have no other reason to change together.
 */
class PreparedReview final
{
public:
    using IntentChecker = std::function<bool()>;
    using UiDispatcher = std::function<void(std::function<void()>)>;
    using DurableOutcomePresenter = std::function<void(nc::ops::CopyOperationDurableTerminalOutcome)>;
    using Submitter = std::function<void(nc::ops::ReviewedVFSOperationPreflight,
                                         std::shared_ptr<nc::core::OperationSubmissionGate::Ticket>,
                                         nc::ops::CopyOperationSubmissionHooks,
                                         std::shared_ptr<std::atomic_bool>)>;

    struct SubmissionPort final {
        UiDispatcher dispatch_to_ui;
        DurableOutcomePresenter present_durable_outcome;
        nc::ops::ItemStateReportCallback item_status_observer;
        Submitter submit;
    };

    PreparedReview(const PreparedReview &) = delete;
    PreparedReview &operator=(const PreparedReview &) = delete;
    PreparedReview(PreparedReview &&) noexcept = default;
    PreparedReview &operator=(PreparedReview &&) noexcept = default;

    [[nodiscard]] const ReviewPresentation &Presentation() const noexcept { return m_Presentation; }

    /** A decision is terminal for this bound preflight; duplicate UI callbacks cannot mint a second authority. */
    [[nodiscard]] ApprovalResult Approve(bool _approved,
                                         const IntentChecker &_intent_is_current,
                                         nc::core::OperationSubmissionGate &_submission_gate,
                                         SubmissionPort _port)
    {
        if( m_Consumed || !m_Preflight )
            return ApprovalResult::AlreadyConsumed;
        m_Consumed = true;

        if( !_approved )
            return ApprovalResult::Declined;
        if( !_intent_is_current || !_intent_is_current() )
            return ApprovalResult::StaleIntent;
        if( !_port.dispatch_to_ui || !_port.present_durable_outcome || !_port.submit )
            return ApprovalResult::MissingSubmissionPort;

        const auto ticket = _submission_gate.Acquire();
        if( !ticket )
            return ApprovalResult::Cancelled;

        auto reviewed = nc::ops::ReviewedVFSOperationPreflight::Review(
            std::move(*m_Preflight), nc::ops::VFSOperationPreflightReviewDecision::Approved);
        m_Preflight.reset();
        if( !reviewed )
            return ApprovalResult::ReviewFailed;

        auto durable_outcome_delivered = std::make_shared<std::atomic_bool>(false);
        nc::ops::CopyOperationSubmissionHooks hooks;
        hooks.item_status_observer = std::move(_port.item_status_observer);
        hooks.durable_terminal_observer =
            [dispatch_to_ui = std::move(_port.dispatch_to_ui),
             present_durable_outcome = std::move(_port.present_durable_outcome),
             durable_outcome_delivered](const nc::ops::CopyOperationDurableTerminalOutcome &_outcome) mutable {
                if( durable_outcome_delivered->exchange(true, std::memory_order_acq_rel) )
                    return;
                auto owning_outcome = _outcome;
                dispatch_to_ui([present_durable_outcome = std::move(present_durable_outcome),
                                owning_outcome = std::move(owning_outcome)]() mutable {
                    present_durable_outcome(std::move(owning_outcome));
                });
            };

        try {
            _port.submit(
                std::move(*reviewed), std::move(ticket), std::move(hooks), std::move(durable_outcome_delivered));
        } catch( ... ) {
            return ApprovalResult::SubmissionPortFailed;
        }
        return ApprovalResult::Submitted;
    }

private:
    PreparedReview(nc::ops::VFSBoundOperationPreflight _preflight, ReviewPresentation _presentation)
        : m_Preflight(std::move(_preflight)), m_Presentation(std::move(_presentation))
    {
    }

    std::optional<nc::ops::VFSBoundOperationPreflight> m_Preflight;
    ReviewPresentation m_Presentation;
    bool m_Consumed = false;

    friend std::expected<PreparedReview, PreparationError>
    PrepareReviewedDeleteApplicationBoundary(nc::ops::VFSBoundOperationPreflight, bool, bool);
};

/**
 * Converts the exact bound preflight into a review-only application projection. No review authority, ticket,
 * operation, journal receipt, or Pool entry exists on any error path.
 */
[[nodiscard]] inline std::expected<PreparedReview, PreparationError>
PrepareReviewedDeleteApplicationBoundary(nc::ops::VFSBoundOperationPreflight _preflight,
                                         const bool _durable_runtime_available,
                                         const bool _intent_is_current)
{
    if( !_durable_runtime_available )
        return std::unexpected(PreparationError{.code = PreparationErrorCode::UnpersistedRuntime});
    if( !_intent_is_current )
        return std::unexpected(PreparationError{.code = PreparationErrorCode::StaleIntent});

    const auto *const accepted = std::get_if<nc::ops::AcceptedOperationPlan>(&_preflight.Result());
    if( !accepted ) {
        const auto &blocked = std::get<nc::ops::BlockedOperationPlan>(_preflight.Result());
        return std::unexpected(PreparationError{
            .code = PreparationErrorCode::BlockedPreflight,
            .blocker = blocked.Blockers().empty() ? std::nullopt : std::optional{blocked.Blockers().front().code},
        });
    }

    const auto &plan = accepted->Plan();
    const auto &report = accepted->Report();
    const auto &sources = plan.Sources();
    // A Delete plan is the inverse shape of a Copy/Move one: no destination at all
    // (`OperationPlan::Create` refuses one, the same way it refuses a conflict policy), and its
    // accepted items live in `deleted_items`, never in `items`. Every source accounted for, one item
    // each, is still the rule the journal itself imposes - a plan whose report covers fewer or more
    // cannot be recorded as completed.
    bool exact_scope = plan.Type() == nc::ops::OperationPlanType::PermanentDelete && !plan.Destination() &&
                       !plan.ConflictPolicy() && !sources.empty() &&
                       report.deleted_items.size() == sources.size() && report.conflicts.empty() &&
                       report.destructive_effects.empty() && !report.requires_confirmation;

    std::vector<ReviewPresentationItem> presentation_items;
    if( exact_scope ) {
        presentation_items.reserve(report.deleted_items.size());
        // Positional, and provably so rather than assumed: an accepted Delete plan's report can never
        // omit a source at all - unlike Copy's `Skip`, nothing about planning a Delete source can
        // succeed without appending to `deleted_items`, so any failure blocks the whole plan and this
        // is never reached with a `BlockedOperationPlan`. Re-derived anyway, for the same reason the
        // Copy/Move boundary re-derives its own destination rather than trusting the planner blindly.
        for( size_t index = 0; index != report.deleted_items.size() && exact_scope; ++index ) {
            const auto &item = report.deleted_items[index];
            const auto &source = sources[index];
            exact_scope = item.source.provider_id == source.ProviderId().Value() &&
                          item.source.absolute_path == source.AbsolutePath();
            if( exact_scope )
                presentation_items.emplace_back(ReviewPresentationItem{item.source.absolute_path});
        }
    }
    if( !exact_scope )
        return std::unexpected(PreparationError{.code = PreparationErrorCode::UnsupportedScope});

    ReviewPresentation presentation{
        .plan_id = std::string{plan.Id().Value()},
        .items = std::move(presentation_items),
        .estimated_files = report.estimated_files,
        .estimated_bytes = report.estimated_bytes,
        .provider_evidence_count = report.provider_evidence.size(),
        .item_evidence_count = report.item_evidence.size(),
        .access_evidence_count = report.access_evidence.size(),
    };
    presentation.warnings.reserve(report.warnings.size());
    for( const auto &warning : report.warnings )
        presentation.warnings.emplace_back(warning.code);

    return PreparedReview{std::move(_preflight), std::move(presentation)};
}

} // namespace nc::panel::actions::reviewed_delete
