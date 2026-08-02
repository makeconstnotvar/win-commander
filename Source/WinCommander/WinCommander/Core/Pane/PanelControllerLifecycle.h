// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PaneLifecycleProducer.h"
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace nc::core {

class PanelControllerLifecycle;

namespace detail {
class PanelControllerLifecycleTestAccess;
}

enum class PaneNavigationExecution : uint8_t {
    Asynchronous,
    Synchronous
};

/**
 * Main-queue admission facts sampled immediately before one request attempt is sequenced.
 *
 * lifecycle_active_request identifies the current coordinator request. lifecycle_tail_request is
 * only the most recently completed, superseded, or cancelled correlation token. The app-layer
 * probe decides external work from explicit controller worker-slot identities.
 */
struct PanelControllerLifecycleProbeContext {
    std::optional<PaneRequestId> lifecycle_active_request;
    std::optional<PaneRequestId> lifecycle_tail_request;
};

struct PanelControllerLifecycleAdmission {
    bool valid = true;
    bool available = true;
    /** Unrelated loading or a reload queue at its production depth cap. */
    bool has_external_loading_work = false;
};

enum class PanelControllerLifecycleSubmissionStatus : uint8_t {
    Accepted,
    Rejected,
    Deferred,
    SynchronousReentrancyUnsupported,
    Shutdown
};

struct PanelControllerLifecycleSubmissionResult {
    /**
     * Accepted and Rejected mean that the producer sequenced the attempt, not eventual success; an
     * accepted request may already be terminal when Submit returns. Deferred, unsupported sync
     * reentry, and Shutdown have no request id. A deferred attempt is freshly probed and sequenced
     * when the transaction gate drains.
     */
    PanelControllerLifecycleSubmissionStatus status = PanelControllerLifecycleSubmissionStatus::Shutdown;
    std::optional<PaneRequestId> request_id;
    std::optional<PaneRejectionReason> rejection_reason;
    /** Typed admission diagnostic when rejection was caused by a failing probe. */
    std::optional<FileManagerError> rejection_error;
};

/**
 * Pure app-layer coordinator between PanelController admission/scheduling seams and the lifecycle
 * producer. PaneLifecycleProducer remains the only owner of active request state, request ids and
 * event sequences.
 */
class PanelControllerLifecycle final
{
public:
    using Observer = PaneLifecycleProducer::Observer;
    using ObservationTicket = PaneLifecycleProducer::ObservationTicket;
    using Subscription = PaneLifecycleProducer::Subscription;
    /** Sampled at actual admission time; exceptions become typed Unavailable rejections. */
    using AdmissionProbe =
        std::function<PanelControllerLifecycleAdmission(const PanelControllerLifecycleProbeContext &)>;
    using Scheduler = std::function<void(PaneRequestId)>;
    /**
     * Main-queue notification for an asynchronous submission that initially returned Deferred.
     * It runs exactly once after that attempt is actually processed. Immediate submissions and
     * deferred attempts discarded by shutdown do not notify it.
     */
    using DeferredResolutionObserver =
        std::function<void(const PanelControllerLifecycleSubmissionResult &)>;
    /**
     * Model-only callback with the strong exception guarantee: throwing must leave observable model
     * state unchanged or fully rolled back. Nested Commit/Fail/Cancel is a contract violation and
     * forces the outer request to publish Failed even when the callback catches the misuse exception.
     */
    using CommitMutation = std::function<void()>;
    using ExceptionMapper = std::function<FileManagerError(std::exception_ptr)>;

    PanelControllerLifecycle(PaneId _pane_id, ExceptionMapper _exception_mapper);
    PanelControllerLifecycle(const PanelControllerLifecycle &) = delete;
    PanelControllerLifecycle(PanelControllerLifecycle &&) = delete;
    ~PanelControllerLifecycle();

    PanelControllerLifecycle &operator=(const PanelControllerLifecycle &) = delete;
    PanelControllerLifecycle &operator=(PanelControllerLifecycle &&) = delete;

    /**
     * Submits navigation according to the production migration policy. Asynchronous navigation is
     * busy behind navigation and supersedes refresh; synchronous navigation supersedes either.
     * Reentrant synchronous submission is unsupported because no synchronous result can be returned
     * after deferred execution; it receives no producer identity or event.
     */
    [[nodiscard]] PanelControllerLifecycleSubmissionResult SubmitNavigation(
        PaneRequestDescriptor _descriptor,
        PaneNavigationExecution _execution,
        AdmissionProbe _probe,
        Scheduler _scheduler,
        DeferredResolutionObserver _deferred_resolution = {});

    /** Refresh is busy behind navigation and latest-wins behind refresh. */
    [[nodiscard]] PanelControllerLifecycleSubmissionResult SubmitRefresh(PaneRequestDescriptor _descriptor,
                                                                         AdmissionProbe _probe,
                                                                         Scheduler _scheduler,
                                                                         DeferredResolutionObserver
                                                                             _deferred_resolution = {});

    /**
     * Preallocates the supplied terminal payload, runs the model mutation only for the active
     * request, then publishes Committed. A throwing mutation leaves producer state unchanged and is
     * converted to exactly one Failed terminal.
     */
    [[nodiscard]] PaneLifecycleProducer::FinishResult Commit(PaneRequestId _request_id,
                                                             PaneLifecycleCommitted _committed,
                                                             CommitMutation _mutation);

    [[nodiscard]] PaneLifecycleProducer::FinishResult Fail(PaneRequestId _request_id,
                                                           FileManagerError _error);
    [[nodiscard]] PaneLifecycleProducer::FinishResult Cancel(
        PaneRequestId _request_id,
        PaneCancellationReason _reason = PaneCancellationReason::InternalAbort);

    /**
     * Requests terminal shutdown. Reentrant shutdown waits for the current transaction and preempts
     * deferred attempts, which have not received producer identities.
     */
    void Shutdown();

    [[nodiscard]] std::optional<PaneActiveRequest> Active() const;
    [[nodiscard]] PaneId Pane() const noexcept;
    [[nodiscard]] ObservationTicket Observe(Observer _observer);
    [[nodiscard]] Subscription Subscribe(Observer _observer);

private:
    friend class detail::PanelControllerLifecycleTestAccess;

    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

namespace detail {

/** Narrow shutdown retry state seam for deterministic coordinator contract tests. */
class PanelControllerLifecycleTestAccess final
{
public:
    [[nodiscard]] static bool ShutdownPerformed(const PanelControllerLifecycle &_lifecycle);
};

} // namespace detail

} // namespace nc::core
