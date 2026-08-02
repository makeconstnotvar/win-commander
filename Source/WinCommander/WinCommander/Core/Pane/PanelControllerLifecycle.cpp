// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelControllerLifecycle.h"

#include <Base/dispatch_cpp.h>
#include <cassert>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nc::core {

namespace {

[[nodiscard]] FileManagerError ExceptionMapperFallback()
{
    return FileManagerError{
        .code = {.domain = "PanelControllerLifecycle", .value = 1},
        .category = FileManagerErrorCategory::UnknownError,
        .severity = FileManagerErrorSeverity::BlockingError,
        .user_message_key = std::string{file_manager_error_messages::UnknownErrorKey},
        .user_message = std::string{file_manager_error_messages::UnknownErrorFallback},
        .technical_message = "PanelControllerLifecycle exception mapper failed.",
        .original_error = nc::Error{"PanelControllerLifecycle", 1},
        .timestamp = std::chrono::system_clock::now(),
    };
}

} // namespace

class PanelControllerLifecycle::Impl final
{
public:
    Impl(const PaneId _pane_id, ExceptionMapper _exception_mapper)
        : m_Producer(_pane_id), m_ExceptionMapper(std::move(_exception_mapper))
    {
        dispatch_assert_main_queue();
        if( !m_ExceptionMapper )
            throw std::invalid_argument("PanelControllerLifecycle requires an exception mapper");
    }

    ~Impl() = default;

    PanelControllerLifecycleSubmissionResult SubmitNavigation(PaneRequestDescriptor _descriptor,
                                                              const PaneNavigationExecution _execution,
                                                              AdmissionProbe _probe,
                                                              Scheduler _scheduler,
                                                              DeferredResolutionObserver
                                                                  _deferred_resolution)
    {
        dispatch_assert_main_queue();
        _descriptor.kind = PaneRequestKind::Navigation;
        return Submit(Submission{
            .descriptor = std::move(_descriptor),
            .synchronous = _execution == PaneNavigationExecution::Synchronous,
            .probe = std::move(_probe),
            .scheduler = std::move(_scheduler),
            .deferred_resolution = std::move(_deferred_resolution),
        });
    }

    PanelControllerLifecycleSubmissionResult SubmitRefresh(PaneRequestDescriptor _descriptor,
                                                           AdmissionProbe _probe,
                                                           Scheduler _scheduler,
                                                           DeferredResolutionObserver
                                                               _deferred_resolution)
    {
        dispatch_assert_main_queue();
        _descriptor.kind = PaneRequestKind::Refresh;
        return Submit(Submission{
            .descriptor = std::move(_descriptor),
            .synchronous = false,
            .probe = std::move(_probe),
            .scheduler = std::move(_scheduler),
            .deferred_resolution = std::move(_deferred_resolution),
        });
    }

    PaneLifecycleProducer::FinishResult Commit(const PaneRequestId _request_id,
                                               PaneLifecycleCommitted _committed,
                                               CommitMutation _mutation)
    {
        dispatch_assert_main_queue();
        if( m_Phase == Phase::CommitMutation ) {
            m_MutationViolated = true;
            throw std::logic_error("PanelControllerLifecycle does not allow nested commit mutation");
        }
        return RunTransaction([&] {
            if( m_ShutdownRequested )
                return PaneLifecycleProducer::FinishResult::ProducerShutdown;

            m_MutationViolated = false;
            try {
                const auto result = m_Producer.Finish(
                    _request_id,
                    std::move(_committed),
                    [&] {
                        m_Phase = Phase::CommitMutation;
                        try {
                            _mutation();
                        } catch( ... ) {
                            m_Phase = Phase::Idle;
                            throw;
                        }
                        m_Phase = Phase::Idle;
                        if( m_MutationViolated ) {
                            m_MutationViolated = false;
                            throw std::logic_error(
                                "PanelControllerLifecycle commit mutation attempted lifecycle reentry");
                        }
                    });
                if( result == PaneLifecycleProducer::FinishResult::Published )
                    m_LifecycleTailRequest = _request_id;
                return result;
            } catch( ... ) {
                m_Phase = Phase::Idle;
                m_MutationViolated = false;
                return FinishException(_request_id, std::current_exception());
            }
        });
    }

    PaneLifecycleProducer::FinishResult Fail(const PaneRequestId _request_id, FileManagerError _error)
    {
        dispatch_assert_main_queue();
        if( m_Phase == Phase::CommitMutation ) {
            m_MutationViolated = true;
            throw std::logic_error("PanelControllerLifecycle cannot fail during a commit mutation");
        }
        return RunTransaction([&] {
            if( m_ShutdownRequested )
                return PaneLifecycleProducer::FinishResult::ProducerShutdown;
            const auto result = m_Producer.Finish(_request_id, PaneLifecycleFailed{std::move(_error)});
            if( result == PaneLifecycleProducer::FinishResult::Published )
                m_LifecycleTailRequest = _request_id;
            return result;
        });
    }

    PaneLifecycleProducer::FinishResult Cancel(const PaneRequestId _request_id,
                                               const PaneCancellationReason _reason)
    {
        dispatch_assert_main_queue();
        if( m_Phase == Phase::CommitMutation ) {
            m_MutationViolated = true;
            throw std::logic_error("PanelControllerLifecycle cannot cancel during a commit mutation");
        }
        return RunTransaction([&] {
            if( m_ShutdownRequested )
                return PaneLifecycleProducer::FinishResult::ProducerShutdown;
            const auto result = m_Producer.Finish(_request_id, PaneLifecycleCancelled{_reason});
            if( result == PaneLifecycleProducer::FinishResult::Published )
                m_LifecycleTailRequest = _request_id;
            return result;
        });
    }

    void Shutdown()
    {
        dispatch_assert_main_queue();
        if( m_ShutdownPerformed )
            return;

        if( !m_ShutdownRequested ) {
            m_ShutdownRequested = true;
            m_DeferredSubmissions.clear();
        }
        if( m_TransactionDepth != 0 || m_IsDraining ) {
            return;
        }

        PerformShutdown();
    }

    [[nodiscard]] std::optional<PaneActiveRequest> Active() const { return m_Producer.Active(); }
    [[nodiscard]] PaneId Pane() const noexcept { return m_Producer.Pane(); }
    [[nodiscard]] ObservationTicket Observe(Observer _observer)
    {
        return m_Producer.Observe(std::move(_observer));
    }
    [[nodiscard]] Subscription Subscribe(Observer _observer)
    {
        return m_Producer.Subscribe(std::move(_observer));
    }

private:
    struct Submission {
        PaneRequestDescriptor descriptor;
        bool synchronous = false;
        AdmissionProbe probe;
        Scheduler scheduler;
        DeferredResolutionObserver deferred_resolution;
    };

    enum class Phase : uint8_t {
        Idle,
        CommitMutation
    };

    PanelControllerLifecycleSubmissionResult Submit(Submission _submission)
    {
        if( m_ShutdownRequested )
            return {.status = PanelControllerLifecycleSubmissionStatus::Shutdown};
        if( !_submission.probe )
            throw std::invalid_argument("PanelControllerLifecycle requires an admission probe");
        if( !_submission.scheduler )
            throw std::invalid_argument("PanelControllerLifecycle requires a scheduler");

        if( m_TransactionDepth != 0 || m_IsDraining ) {
            if( _submission.synchronous ) {
                return {
                    .status = PanelControllerLifecycleSubmissionStatus::SynchronousReentrancyUnsupported,
                };
            }
            m_DeferredSubmissions.emplace_back(std::move(_submission));
            return {.status = PanelControllerLifecycleSubmissionStatus::Deferred};
        }

        return RunTransaction([&] { return ProcessSubmission(std::move(_submission)); });
    }

    PanelControllerLifecycleSubmissionResult ProcessSubmission(Submission _submission)
    {
        if( m_ShutdownPerformed )
            return {.status = PanelControllerLifecycleSubmissionStatus::Shutdown};

        const auto active_before_probe = m_Producer.Active();
        const PanelControllerLifecycleProbeContext context{
            .lifecycle_active_request = active_before_probe ? std::optional{active_before_probe->request_id}
                                                            : std::nullopt,
            .lifecycle_tail_request = m_LifecycleTailRequest,
        };
        PanelControllerLifecycleAdmission admission;
        try {
            admission = _submission.probe(context);
        } catch( ... ) {
            if( m_ShutdownRequested )
                return {.status = PanelControllerLifecycleSubmissionStatus::Shutdown};
            auto admission_error = MapException(std::current_exception());
            if( m_ShutdownRequested )
                return {.status = PanelControllerLifecycleSubmissionStatus::Shutdown};
            return Reject(std::move(_submission.descriptor),
                          PaneRejectionReason::Unavailable,
                          std::nullopt,
                          std::move(admission_error));
        }

        // A probe can reentrantly request shutdown. It is not an accepted lifecycle attempt.
        if( m_ShutdownRequested )
            return {.status = PanelControllerLifecycleSubmissionStatus::Shutdown};

        if( !admission.valid )
            return Reject(std::move(_submission.descriptor), PaneRejectionReason::InvalidRequest, std::nullopt);
        if( !admission.available )
            return Reject(std::move(_submission.descriptor), PaneRejectionReason::Unavailable, std::nullopt);

        const auto active = m_Producer.Active();
        if( admission.has_external_loading_work ) {
            return Reject(std::move(_submission.descriptor),
                          PaneRejectionReason::Busy,
                          std::nullopt);
        }

        bool supersede = false;
        if( active ) {
            if( _submission.descriptor.kind == PaneRequestKind::Navigation ) {
                if( _submission.synchronous || active->descriptor.kind == PaneRequestKind::Refresh )
                    supersede = true;
                else
                    return Reject(std::move(_submission.descriptor),
                                  PaneRejectionReason::Busy,
                                  active->request_id);
            }
            else {
                if( active->descriptor.kind == PaneRequestKind::Refresh )
                    supersede = true;
                else
                    return Reject(std::move(_submission.descriptor),
                                  PaneRejectionReason::Busy,
                                  active->request_id);
            }
        }

        PaneRequestId request_id;
        if( supersede ) {
            assert(active);
            request_id = m_Producer.SupersedeAndStart(std::move(_submission.descriptor));
            m_LifecycleTailRequest = active->request_id;
        }
        else {
            request_id = m_Producer.Start(std::move(_submission.descriptor));
            m_LifecycleTailRequest.reset();
        }

        // Started observers can synchronously cancel, request shutdown or destroy the facade.
        // Only the producer's current identity decides whether scheduling is still valid.
        const auto active_before_scheduler = m_Producer.Active();
        if( !m_ShutdownRequested && active_before_scheduler &&
            active_before_scheduler->request_id == request_id ) {
            try {
                _submission.scheduler(request_id);
            } catch( ... ) {
                [[maybe_unused]] const auto failure = FinishException(request_id, std::current_exception());
            }
        }

        return {
            .status = PanelControllerLifecycleSubmissionStatus::Accepted,
            .request_id = request_id,
        };
    }

    PanelControllerLifecycleSubmissionResult Reject(PaneRequestDescriptor _descriptor,
                                                     const PaneRejectionReason _reason,
                                                     std::optional<PaneRequestId> _conflicting_request,
                                                     std::optional<FileManagerError> _admission_error = std::nullopt)
    {
        auto result_error = _admission_error;
        const PaneRequestId request_id = m_Producer.Reject(
            std::move(_descriptor),
            PaneLifecycleRejected{.reason = _reason,
                                  .conflicting_request = _conflicting_request,
                                  .admission_error = std::move(_admission_error)});
        return {
            .status = PanelControllerLifecycleSubmissionStatus::Rejected,
            .request_id = request_id,
            .rejection_reason = _reason,
            .rejection_error = std::move(result_error),
        };
    }

    PaneLifecycleProducer::FinishResult FinishException(const PaneRequestId _request_id,
                                                        std::exception_ptr _exception)
    {
        const auto active = m_Producer.Active();
        if( !active )
            return m_ShutdownRequested ? PaneLifecycleProducer::FinishResult::ProducerShutdown
                                       : PaneLifecycleProducer::FinishResult::NoActiveRequest;
        if( active->request_id != _request_id )
            return PaneLifecycleProducer::FinishResult::StaleRequest;

        FileManagerError error = MapException(std::move(_exception));
        const auto result = m_Producer.Finish(_request_id, PaneLifecycleFailed{std::move(error)});
        if( result == PaneLifecycleProducer::FinishResult::Published )
            m_LifecycleTailRequest = _request_id;
        return result;
    }

    [[nodiscard]] FileManagerError MapException(std::exception_ptr _exception)
    {
        try {
            return m_ExceptionMapper(std::move(_exception));
        } catch( ... ) {
            return ExceptionMapperFallback();
        }
    }

    void PerformShutdown()
    {
        if( m_ShutdownPerformed )
            return;
        if( const auto active = m_Producer.Active() )
            m_LifecycleTailRequest = active->request_id;
        m_Producer.Shutdown();
        m_ShutdownPerformed = true;
    }

    void DrainDeferredActions()
    {
        if( m_IsDraining || m_TransactionDepth != 0 )
            return;

        m_IsDraining = true;
        std::exception_ptr first_failure;
        const auto capture_failure = [&] {
            if( !first_failure )
                first_failure = std::current_exception();
        };
        const auto finish_shutdown = [&] {
            m_DeferredSubmissions.clear();
            try {
                PerformShutdown();
            } catch( ... ) {
                capture_failure();
            }
        };

        try {
            if( m_ShutdownRequested )
                finish_shutdown();
            while( !m_DeferredSubmissions.empty() ) {
                Submission submission = std::move(m_DeferredSubmissions.front());
                m_DeferredSubmissions.pop_front();
                DeferredResolutionObserver resolution = std::move(submission.deferred_resolution);
                ++m_TransactionDepth;
                try {
                    const auto result = ProcessSubmission(std::move(submission));
                    if( resolution )
                        resolution(result);
                } catch( ... ) {
                    capture_failure();
                }
                --m_TransactionDepth;
                if( m_ShutdownRequested )
                    finish_shutdown();
            }
        } catch( ... ) {
            capture_failure();
            if( m_ShutdownRequested )
                finish_shutdown();
            m_IsDraining = false;
            if( first_failure )
                std::rethrow_exception(first_failure);
        }
        m_IsDraining = false;
        if( first_failure )
            std::rethrow_exception(first_failure);
    }

    template <class Callback>
    auto RunTransaction(Callback &&_callback) -> std::invoke_result_t<Callback>
    {
        using Result = std::invoke_result_t<Callback>;
        ++m_TransactionDepth;
        if constexpr( std::is_void_v<Result> ) {
            try {
                std::forward<Callback>(_callback)();
            }
            catch( ... ) {
                --m_TransactionDepth;
                throw;
            }
            --m_TransactionDepth;
            if( m_TransactionDepth == 0 && !m_IsDraining )
                DrainDeferredActions();
        }
        else {
            Result result = [&]() -> Result {
                try {
                    return std::forward<Callback>(_callback)();
                } catch( ... ) {
                    --m_TransactionDepth;
                    throw;
                }
            }();
            --m_TransactionDepth;
            if( m_TransactionDepth == 0 && !m_IsDraining )
                DrainDeferredActions();
            return result;
        }
    }

    PaneLifecycleProducer m_Producer;
    ExceptionMapper m_ExceptionMapper;
    std::deque<Submission> m_DeferredSubmissions;
    std::optional<PaneRequestId> m_LifecycleTailRequest;
    std::size_t m_TransactionDepth = 0;
    Phase m_Phase = Phase::Idle;
    bool m_MutationViolated = false;
    bool m_IsDraining = false;
    bool m_ShutdownRequested = false;
    bool m_ShutdownPerformed = false;

    friend class detail::PanelControllerLifecycleTestAccess;
};

PanelControllerLifecycle::PanelControllerLifecycle(PaneId _pane_id, ExceptionMapper _exception_mapper)
    : m_Impl(std::make_shared<Impl>(_pane_id, std::move(_exception_mapper)))
{
}

PanelControllerLifecycle::~PanelControllerLifecycle()
{
    const auto impl = m_Impl;
    if( impl ) {
        try {
            impl->Shutdown();
        } catch( ... ) {
            // PaneLifecycleProducer's destruction path retains the final no-throw fallback.
        }
    }
}

PanelControllerLifecycleSubmissionResult PanelControllerLifecycle::SubmitNavigation(
    PaneRequestDescriptor _descriptor,
    const PaneNavigationExecution _execution,
    AdmissionProbe _probe,
    Scheduler _scheduler,
    DeferredResolutionObserver _deferred_resolution)
{
    const auto impl = m_Impl;
    return impl->SubmitNavigation(std::move(_descriptor),
                                  _execution,
                                  std::move(_probe),
                                  std::move(_scheduler),
                                  std::move(_deferred_resolution));
}

PanelControllerLifecycleSubmissionResult PanelControllerLifecycle::SubmitRefresh(
    PaneRequestDescriptor _descriptor,
    AdmissionProbe _probe,
    Scheduler _scheduler,
    DeferredResolutionObserver _deferred_resolution)
{
    const auto impl = m_Impl;
    return impl->SubmitRefresh(std::move(_descriptor),
                               std::move(_probe),
                               std::move(_scheduler),
                               std::move(_deferred_resolution));
}

PaneLifecycleProducer::FinishResult PanelControllerLifecycle::Commit(const PaneRequestId _request_id,
                                                                    PaneLifecycleCommitted _committed,
                                                                    CommitMutation _mutation)
{
    const auto impl = m_Impl;
    return impl->Commit(_request_id, std::move(_committed), std::move(_mutation));
}

PaneLifecycleProducer::FinishResult PanelControllerLifecycle::Fail(const PaneRequestId _request_id,
                                                                  FileManagerError _error)
{
    const auto impl = m_Impl;
    return impl->Fail(_request_id, std::move(_error));
}

PaneLifecycleProducer::FinishResult PanelControllerLifecycle::Cancel(const PaneRequestId _request_id,
                                                                    const PaneCancellationReason _reason)
{
    const auto impl = m_Impl;
    return impl->Cancel(_request_id, _reason);
}

void PanelControllerLifecycle::Shutdown()
{
    const auto impl = m_Impl;
    impl->Shutdown();
}

std::optional<PaneActiveRequest> PanelControllerLifecycle::Active() const
{
    const auto impl = m_Impl;
    return impl->Active();
}

PaneId PanelControllerLifecycle::Pane() const noexcept
{
    const auto impl = m_Impl;
    return impl->Pane();
}

PanelControllerLifecycle::ObservationTicket PanelControllerLifecycle::Observe(Observer _observer)
{
    const auto impl = m_Impl;
    return impl->Observe(std::move(_observer));
}

PanelControllerLifecycle::Subscription PanelControllerLifecycle::Subscribe(Observer _observer)
{
    const auto impl = m_Impl;
    return impl->Subscribe(std::move(_observer));
}

bool detail::PanelControllerLifecycleTestAccess::ShutdownPerformed(
    const PanelControllerLifecycle &_lifecycle)
{
    return _lifecycle.m_Impl->m_ShutdownPerformed;
}

} // namespace nc::core
