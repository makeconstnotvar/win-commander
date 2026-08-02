// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PaneSnapshot.h"
#include <Base/ScopedObservable.h>
#include <WinCommander/Core/Errors/FileManagerError.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace nc::core {

struct PaneRequestId {
    uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    constexpr bool operator==(const PaneRequestId &) const noexcept = default;
};

enum class PaneRequestKind : uint8_t {
    Navigation,
    Refresh
};

struct PaneRequestLocation {
    VFSHostPtr host;
    std::string path;

    bool operator==(const PaneRequestLocation &) const noexcept = default;
};

/** Immutable request metadata copied into every event belonging to one attempt. */
struct PaneRequestDescriptor {
    PaneRequestKind kind = PaneRequestKind::Navigation;
    std::optional<PaneRequestLocation> target;
    bool initiated_by_user = false;

    bool operator==(const PaneRequestDescriptor &) const noexcept = default;
};

struct PaneLifecycleStarted {
    bool operator==(const PaneLifecycleStarted &) const noexcept = default;
};

/** Identity of the model state that was committed before this event was published. */
struct PaneLifecycleCommitted {
    uint64_t controller_generation = 0;
    VFSListingPtr listing;

    bool operator==(const PaneLifecycleCommitted &) const noexcept = default;
};

struct PaneLifecycleFailed {
    FileManagerError error;
};

enum class PaneCancellationReason : uint8_t {
    User,
    QueueStopped,
    ProducerShutdown,
    InternalAbort
};

struct PaneLifecycleCancelled {
    PaneCancellationReason reason = PaneCancellationReason::InternalAbort;

    bool operator==(const PaneLifecycleCancelled &) const noexcept = default;
};

struct PaneLifecycleSuperseded {
    PaneRequestId replacement;

    bool operator==(const PaneLifecycleSuperseded &) const noexcept = default;
};

enum class PaneRejectionReason : uint8_t {
    Busy,
    InvalidRequest,
    Unavailable
};

struct PaneLifecycleRejected {
    PaneRejectionReason reason = PaneRejectionReason::Unavailable;
    std::optional<PaneRequestId> conflicting_request;
    /** Typed diagnostic when admission itself failed; policy rejections leave it empty. */
    std::optional<FileManagerError> admission_error;

    bool operator==(const PaneLifecycleRejected &) const noexcept = default;
};

/** Payloads accepted by Finish; Superseded is published only by SupersedeAndStart. */
using PaneLifecycleFinishPayload =
    std::variant<PaneLifecycleCommitted, PaneLifecycleFailed, PaneLifecycleCancelled>;
using PaneLifecycleEventPayload = std::variant<PaneLifecycleStarted,
                                               PaneLifecycleCommitted,
                                               PaneLifecycleFailed,
                                               PaneLifecycleCancelled,
                                               PaneLifecycleSuperseded,
                                               PaneLifecycleRejected>;

/** True for the exactly-once terminal outcome of an accepted request. */
[[nodiscard]] inline bool IsPaneLifecycleAcceptedTerminal(const PaneLifecycleEventPayload &_payload) noexcept
{
    return std::holds_alternative<PaneLifecycleCommitted>(_payload) ||
           std::holds_alternative<PaneLifecycleFailed>(_payload) ||
           std::holds_alternative<PaneLifecycleCancelled>(_payload) ||
           std::holds_alternative<PaneLifecycleSuperseded>(_payload);
}

struct PaneLifecycleEvent {
    PaneId pane_id;
    PaneRequestId request_id;
    uint64_t event_sequence = 0;
    PaneRequestDescriptor descriptor;
    PaneLifecycleEventPayload payload;
};

struct PaneActiveRequest {
    PaneRequestId request_id;
    PaneRequestDescriptor descriptor;

    bool operator==(const PaneActiveRequest &) const noexcept = default;
};

namespace detail {

struct PaneLifecycleCounterLimits {
    uint64_t maximum_request_id = std::numeric_limits<uint64_t>::max();
    uint64_t maximum_event_sequence = std::numeric_limits<uint64_t>::max();
};

class PaneLifecycleProducerTestAccess;

} // namespace detail

/**
 * Main-queue sequencer for one pane's authoritative loading lifecycle.
 *
 * Accepted requests publish Started followed by exactly one terminal event. Rejected attempts have
 * their own stable request id and publish Rejected without becoming active. Observer callbacks are
 * synchronous, non-throwing main-queue callbacks; reentrant publications are drained in FIFO order.
 */
class PaneLifecycleProducer final
{
public:
    using Observer = std::function<void(const PaneLifecycleEvent &)>;
    using ObservationTicket = base::ScopedObservableBase::ObservationTicket;
    using FinishMutation = std::function<void()>;

    /**
     * Linearizable observation boundary for a reducer joining an already-running producer.
     * seed_request is either the current accepted request or the request needed to reduce the
     * retained failure. For retained-failure replay, checkpoint_sequence records the last event
     * that the newly registered observer will not receive while retained_failure preserves its
     * original event identity. Active-request seeds intentionally leave the checkpoint at zero.
     */
    struct Subscription {
        ObservationTicket observation;
        std::optional<PaneActiveRequest> seed_request;
        std::optional<PaneLifecycleEvent> retained_failure;
        uint64_t checkpoint_sequence = 0;
    };

    enum class FinishResult : uint8_t {
        Published,
        NoActiveRequest,
        StaleRequest,
        ProducerShutdown
    };

    explicit PaneLifecycleProducer(PaneId _pane_id);
    PaneLifecycleProducer(const PaneLifecycleProducer &) = delete;
    PaneLifecycleProducer(PaneLifecycleProducer &&) = delete;
    ~PaneLifecycleProducer();

    PaneLifecycleProducer &operator=(const PaneLifecycleProducer &) = delete;
    PaneLifecycleProducer &operator=(PaneLifecycleProducer &&) = delete;

    /** Starts an accepted request. Throws std::logic_error if another request is active or after shutdown. */
    [[nodiscard]] PaneRequestId Start(PaneRequestDescriptor _descriptor);

    /** Publishes an unaccepted attempt without changing the active request. */
    [[nodiscard]] PaneRequestId Reject(PaneRequestDescriptor _descriptor, PaneLifecycleRejected _rejection);

    /** Atomically supersedes the active request and starts its replacement. */
    [[nodiscard]] PaneRequestId SupersedeAndStart(PaneRequestDescriptor _descriptor);

    /** Publishes one terminal event; delayed and duplicate completions are suppressed. */
    [[nodiscard]] FinishResult Finish(PaneRequestId _request_id, PaneLifecycleFinishPayload _terminal);

    /**
     * Validates and preallocates the terminal event before invoking a strong-exception-safe mutation.
     * A throwing mutation leaves active state, counters and pending notifications unchanged. Any
     * lifecycle mutation reentry is rejected and forces the outer mutation to throw even if caught.
     */
    [[nodiscard]] FinishResult Finish(PaneRequestId _request_id,
                                      PaneLifecycleFinishPayload _terminal,
                                      FinishMutation _mutation);

    /** Cancels an active request with ProducerShutdown and prevents subsequent submissions. */
    void Shutdown();

    [[nodiscard]] std::optional<PaneActiveRequest> Active() const;
    [[nodiscard]] PaneId Pane() const noexcept;
    [[nodiscard]] ObservationTicket Observe(Observer _observer);
    [[nodiscard]] Subscription Subscribe(Observer _observer);

private:
    friend class detail::PaneLifecycleProducerTestAccess;

    PaneLifecycleProducer(PaneId _pane_id, detail::PaneLifecycleCounterLimits _counter_limits);

    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

namespace detail {

/** Narrow deterministic overflow seam for the producer contract tests. */
class PaneLifecycleProducerTestAccess final
{
public:
    [[nodiscard]] static std::unique_ptr<PaneLifecycleProducer> Make(
        PaneId _pane_id,
        PaneLifecycleCounterLimits _counter_limits);
};

} // namespace detail

} // namespace nc::core
