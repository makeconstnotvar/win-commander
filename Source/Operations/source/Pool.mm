// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Pool.h"
#include "Operation.h"
#include <Base/dispatch_cpp.h>
#include <algorithm>
#include <exception>
#include <iostream>
#include <thread>

namespace nc::ops {

std::shared_ptr<Pool> Pool::Make()
{
    struct workaround : public Pool {
    };
    return std::make_shared<workaround>();
}

Pool::Pool() = default;

Pool::~Pool() = default;

void Pool::Enqueue(std::shared_ptr<Operation> _operation)
{
    (void)TryEnqueue(std::move(_operation));
}

PoolEnqueueResult Pool::TryEnqueue(std::shared_ptr<Operation> _operation, TerminalFinalizer _terminal_finalizer)
{
    if( !_operation )
        return PoolEnqueueResult::NotCold;

    {
        const auto guard = std::lock_guard{m_Lock};
        if( m_ShuttingDown )
            return PoolEnqueueResult::ShuttingDown;
        const auto is_same_operation = [&](const auto &_candidate) { return _candidate.get() == _operation.get(); };
        const bool active_duplicate = std::ranges::any_of(m_RunningOperations, is_same_operation) ||
                                      std::ranges::any_of(m_PendingOperations, is_same_operation);
        const bool finalizing_duplicate = std::ranges::any_of(m_FinalizingOperations, [&](const auto &_candidate) {
            return _candidate->operation.get() == _operation.get();
        });
        if( active_duplicate || finalizing_duplicate )
            return PoolEnqueueResult::Duplicate;
        if( _operation->State() != OperationState::Cold )
            return PoolEnqueueResult::NotCold;

        auto prepared_finalizer = std::make_shared<FinalizingOperation>();
        prepared_finalizer->operation = _operation;
        prepared_finalizer->finalizer = std::move(_terminal_finalizer);
        const auto weak_this = std::weak_ptr<Pool>{shared_from_this()};
        const auto weak_operation = std::weak_ptr<Operation>{_operation};

        m_FinalizingOperations.reserve(m_FinalizingOperations.size() + m_TerminalFinalizers.size() + 1);
        m_TerminalFinalizers.reserve(m_TerminalFinalizers.size() + 1);
        m_PendingOperations.push_back(_operation);
        try {
            prepared_finalizer->finish_observation =
                _operation->Observe(Operation::NotifyAboutFinish, [weak_this, weak_operation] {
                    const auto pool = weak_this.lock();
                    const auto op = weak_operation.lock();
                    if( pool && op )
                        pool->OperationDidFinish(op);
                });
            prepared_finalizer->start_observation =
                _operation->Observe(Operation::NotifyAboutStart, [weak_this, weak_operation] {
                    const auto pool = weak_this.lock();
                    const auto op = weak_operation.lock();
                    if( pool && op )
                        pool->OperationDidStart(op);
                });
            m_TerminalFinalizers.emplace_back(prepared_finalizer);
            _operation->SetDialogCallback([weak_this](NSWindow *_dlg, std::function<void(NSModalResponse)> _cb) {
                if( const auto pool = weak_this.lock() )
                    return pool->ShowDialog(_dlg, _cb);
                return false;
            });
        } catch( ... ) {
            if( !m_TerminalFinalizers.empty() && m_TerminalFinalizers.back() == prepared_finalizer )
                m_TerminalFinalizers.pop_back();
            m_PendingOperations.pop_back();
            throw;
        }
    }

    try {
        FireObservers(NotifyAboutAddition);
    } catch( const std::exception &e ) {
        std::cerr << "Error: Pool addition observer has thrown an exception after admission: " << e.what() << ".\n";
    } catch( ... ) {
        std::cerr << "Error: Pool addition observer has thrown an unknown exception after admission.\n";
    }
    try {
        StartPendingOperations();
    } catch( const std::exception &e ) {
        std::cerr << "Error: Pool could not start admitted work: " << e.what() << ".\n";
    } catch( ... ) {
        std::cerr << "Error: Pool could not start admitted work due to an unknown exception.\n";
    }
    return PoolEnqueueResult::Accepted;
}

void Pool::OperationDidStart([[maybe_unused]] const std::shared_ptr<Operation> &_operation)
{
}

void Pool::OperationDidFinish([[maybe_unused]] const std::shared_ptr<Operation> &_operation)
{
    std::shared_ptr<FinalizingOperation> finalizing;
    {
        const auto guard = std::lock_guard{m_Lock};
        const auto already_finalizing = std::ranges::find_if(
            m_FinalizingOperations, [&](const auto &_candidate) { return _candidate->operation == _operation; });
        if( already_finalizing != m_FinalizingOperations.end() )
            return;

        const bool was_running = std::ranges::find(m_RunningOperations, _operation) != m_RunningOperations.end();
        const bool was_pending = std::ranges::find(m_PendingOperations, _operation) != m_PendingOperations.end();
        if( !was_running && !was_pending )
            return;

        const auto registered = std::ranges::find_if(
            m_TerminalFinalizers, [&](const auto &_candidate) { return _candidate->operation == _operation; });
        if( registered == m_TerminalFinalizers.end() )
            return;
        finalizing = *registered;
        m_FinalizingOperations.emplace_back(finalizing);
        std::erase(m_RunningOperations, _operation);
        std::erase(m_PendingOperations, _operation);
        m_TerminalFinalizers.erase(registered);
    }

    const auto finalization = RetryFinalization(_operation);
    if( finalization == PoolRetryFinalizationResult::Retained ||
        finalization == PoolRetryFinalizationResult::InProgress )
        StartPendingOperations();
}

PoolRetryFinalizationResult Pool::RetryFinalization(const std::shared_ptr<Operation> &_operation)
{
    if( !_operation )
        return PoolRetryFinalizationResult::NotFinalizing;

    std::shared_ptr<FinalizingOperation> finalizing;
    {
        const auto guard = std::lock_guard{m_Lock};
        const auto found = std::ranges::find_if(
            m_FinalizingOperations, [&](const auto &_candidate) { return _candidate->operation == _operation; });
        if( found == m_FinalizingOperations.end() )
            return PoolRetryFinalizationResult::NotFinalizing;
        finalizing = *found;
        if( finalizing->in_progress )
            return PoolRetryFinalizationResult::InProgress;
        finalizing->in_progress = true;
    }

    auto decision = PoolTerminalFinalizationDecision::Release;
    if( finalizing->finalizer ) {
        try {
            decision = finalizing->finalizer(finalizing->operation);
        } catch( ... ) {
            decision = PoolTerminalFinalizationDecision::Retain;
        }
    }
    switch( decision ) {
        case PoolTerminalFinalizationDecision::Release:
        case PoolTerminalFinalizationDecision::ReleaseWithoutCompletion:
        case PoolTerminalFinalizationDecision::Retain:
            break;
        default:
            decision = PoolTerminalFinalizationDecision::Retain;
            break;
    }
    if( decision == PoolTerminalFinalizationDecision::Retain ) {
        const auto guard = std::lock_guard{m_Lock};
        const auto found = std::ranges::find(m_FinalizingOperations, finalizing);
        if( found == m_FinalizingOperations.end() )
            return PoolRetryFinalizationResult::NotFinalizing;
        finalizing->in_progress = false;
        return PoolRetryFinalizationResult::Retained;
    }

    {
        const auto guard = std::lock_guard{m_Lock};
        const auto found = std::ranges::find(m_FinalizingOperations, finalizing);
        if( found == m_FinalizingOperations.end() )
            return PoolRetryFinalizationResult::NotFinalizing;
        m_FinalizingOperations.erase(found);
    }
    OperationFinalizationDidRelease(_operation, decision == PoolTerminalFinalizationDecision::Release);
    return PoolRetryFinalizationResult::Released;
}

void Pool::OperationFinalizationDidRelease(const std::shared_ptr<Operation> &_operation, bool _report_completion)
{
    std::exception_ptr first_error;
    try {
        FireObservers(NotifyAboutRemoval);
    } catch( ... ) {
        first_error = std::current_exception();
    }
    try {
        StartPendingOperations();
    } catch( ... ) {
        if( !first_error )
            first_error = std::current_exception();
    }
    try {
        if( _report_completion && _operation->State() == OperationState::Completed && m_OperationCompletionCallback )
            m_OperationCompletionCallback(_operation);
    } catch( ... ) {
        if( !first_error )
            first_error = std::current_exception();
    }
    if( first_error )
        std::rethrow_exception(first_error);
}

void Pool::StartPendingOperations()
{
    std::vector<std::shared_ptr<Operation>> to_start;

    {
        const auto guard = std::lock_guard{m_Lock};
        if( m_ShuttingDown )
            return;

        // 1st - evaluate every callback decision before mutating either container.
        if( m_ShouldBeQueuedCallback ) {
            std::vector<bool> start_without_queue;
            start_without_queue.reserve(m_PendingOperations.size());
            bool decision_failed = false;
            for( const auto &operation : m_PendingOperations ) {
                assert(operation != nullptr);
                bool should_be_queued = true;
                try {
                    should_be_queued = m_ShouldBeQueuedCallback(*operation);
                } catch( const std::exception &e ) {
                    decision_failed = true;
                    std::cerr << "Error: Pool enqueue policy has thrown an exception; keeping the operation queued: "
                              << e.what() << ".\n";
                } catch( ... ) {
                    decision_failed = true;
                    std::cerr << "Error: Pool enqueue policy has thrown an unknown exception; keeping the operation "
                                 "queued.\n";
                }
                start_without_queue.emplace_back(!should_be_queued);
            }
            if( decision_failed )
                std::ranges::fill(start_without_queue, false);

            to_start.reserve(m_PendingOperations.size());
            m_RunningOperations.reserve(m_RunningOperations.size() + m_PendingOperations.size());
            for( size_t index = 0; index < m_PendingOperations.size(); ++index ) {
                auto &operation = m_PendingOperations[index];
                if( start_without_queue[index] ) {
                    to_start.emplace_back(operation);
                    m_RunningOperations.emplace_back(operation);
                    operation.reset();
                }
            }
            std::erase_if(m_PendingOperations, [](const auto &_op) { return _op == nullptr; });
        }

        // 2nd - gather any other operations until the pool has enough running operations
        const auto running_now = static_cast<int>(m_RunningOperations.size());
        auto gathered = 0;
        while( running_now + gathered < m_Concurrency && !m_PendingOperations.empty() ) {
            const auto op = m_PendingOperations.front();
            m_PendingOperations.pop_front();
            to_start.emplace_back(op);
            m_RunningOperations.emplace_back(op);
            ++gathered;
        }
    }

    // now kickstart all these operations
    std::exception_ptr first_error;
    for( const auto &op : to_start ) {
        try {
            const auto start_guard = std::lock_guard{m_StartGate};
            {
                const auto guard = std::lock_guard{m_Lock};
                if( m_ShuttingDown )
                    continue;
            }
            op->Start();
        } catch( ... ) {
            if( !first_error )
                first_error = std::current_exception();
        }
    }
    if( first_error )
        std::rethrow_exception(first_error);
}

Pool::ObservationTicket Pool::Observe(uint64_t _notification_mask, std::function<void()> _callback)
{
    return AddTicketedObserver(std::move(_callback), _notification_mask);
}

void Pool::ObserveUnticketed(uint64_t _notification_mask, std::function<void()> _callback)
{
    AddUnticketedObserver(std::move(_callback), _notification_mask);
}

int Pool::RunningOperationsCount() const
{
    const auto guard = std::lock_guard{m_Lock};
    return static_cast<int>(m_RunningOperations.size());
}

int Pool::FinalizingOperationsCount() const
{
    const auto guard = std::lock_guard{m_Lock};
    return static_cast<int>(m_FinalizingOperations.size());
}

int Pool::OperationsCount() const
{
    const auto guard = std::lock_guard{m_Lock};
    return static_cast<int>(m_RunningOperations.size() + m_PendingOperations.size() + m_FinalizingOperations.size());
}

std::vector<std::shared_ptr<Operation>> Pool::Operations() const
{
    const auto guard = std::lock_guard{m_Lock};
    auto v = m_RunningOperations;
    v.insert(end(v), begin(m_PendingOperations), end(m_PendingOperations));
    for( const auto &finalizing : m_FinalizingOperations )
        v.emplace_back(finalizing->operation);
    return v;
}

std::vector<std::shared_ptr<Operation>> Pool::RunningOperations() const
{
    const auto guard = std::lock_guard{m_Lock};
    return m_RunningOperations;
}

std::vector<std::shared_ptr<Operation>> Pool::FinalizingOperations() const
{
    const auto guard = std::lock_guard{m_Lock};
    std::vector<std::shared_ptr<Operation>> operations;
    operations.reserve(m_FinalizingOperations.size());
    for( const auto &finalizing : m_FinalizingOperations )
        operations.emplace_back(finalizing->operation);
    return operations;
}

void Pool::SetDialogCallback(std::function<void(NSWindow *, std::function<void(NSModalResponse)>)> _callback)
{
    m_DialogPresentation = std::move(_callback);
}

void Pool::SetOperationCompletionCallback(std::function<void(const std::shared_ptr<Operation> &)> _callback)
{
    m_OperationCompletionCallback = std::move(_callback);
}

bool Pool::IsInteractive() const
{
    return m_DialogPresentation != nullptr;
}

bool Pool::ShowDialog(NSWindow *_dialog, std::function<void(NSModalResponse)> _callback)
{
    dispatch_assert_main_queue();
    if( !m_DialogPresentation )
        return false;
    m_DialogPresentation(_dialog, std::move(_callback));
    return true;
}

int Pool::Concurrency()
{
    return m_Concurrency;
}

void Pool::SetConcurrency(int _maximum_current_operations)
{
    m_Concurrency = std::max(_maximum_current_operations, 1);
}

void Pool::SetEnqueuingCallback(std::function<bool(const Operation &_operation)> _should_be_queued)
{
    assert(Empty());
    m_ShouldBeQueuedCallback = std::move(_should_be_queued);
}

bool Pool::Empty() const
{
    const auto guard = std::lock_guard{m_Lock};
    return m_RunningOperations.empty() && m_PendingOperations.empty() && m_FinalizingOperations.empty();
}

void Pool::StopAndWaitForShutdown()
{
    std::vector<std::shared_ptr<Operation>> operations;
    {
        const auto guard = std::lock_guard{m_Lock};
        m_ShuttingDown = true;
        operations = m_RunningOperations;
        operations.insert(end(operations), begin(m_PendingOperations), end(m_PendingOperations));
        for( const auto &finalizing : m_FinalizingOperations )
            operations.emplace_back(finalizing->operation);
    }

    // Wait for any Start() that was already admitted before the shutdown boundary.
    {
        const auto start_guard = std::lock_guard{m_StartGate};
    }

    for( const auto &operation : operations )
        operation->AbortUIWaiting();
    for( const auto &operation : operations )
        operation->Stop();
    for( const auto &operation : operations )
        operation->Wait();
}

} // namespace nc::ops
