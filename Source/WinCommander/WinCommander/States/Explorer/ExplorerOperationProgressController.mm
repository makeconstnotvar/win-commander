// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerOperationProgressController.h"
#include "NCExplorerOperationProgressView.h"

#include <Operations/Operation.h>
#include <Operations/Pool.h>
#include <Operations/Statistics.h>
#include <WinCommander/Core/VisualState/ExplorerOperationProgressModel.h>
#include <algorithm>
#include <unordered_set>

using nc::core::ExplorerOperationLifecycle;
using nc::core::ExplorerOperationProgressInput;
using nc::core::ExplorerOperationProgressModel;
using nc::core::ExplorerOperationProgressSource;
using nc::core::ExplorerOperationProgressUnit;

namespace {

ExplorerOperationProgressSource Source(const nc::ops::Statistics &_statistics,
                                       const nc::ops::Statistics::SourceType _type)
{
    return ExplorerOperationProgressSource{
        .processed = _statistics.VolumeProcessed(_type),
        .total = _statistics.VolumeTotal(_type),
        .speed_per_second = _statistics.SpeedPerSecondAverage(_type),
        .eta = _statistics.ETA(_type),
    };
}

ExplorerOperationLifecycle Lifecycle(const nc::ops::Operation &_operation, const bool _finalizing)
{
    if( _finalizing )
        return ExplorerOperationLifecycle::Finalizing;
    if( _operation.IsWaitingForUIResponse() )
        return ExplorerOperationLifecycle::WaitingForUser;
    using enum nc::ops::OperationState;
    switch( _operation.State() ) {
        case Cold:
            return ExplorerOperationLifecycle::Queued;
        case Running:
            return ExplorerOperationLifecycle::Running;
        case Paused:
            return ExplorerOperationLifecycle::Paused;
        case Stopped:
        case Completed:
            return ExplorerOperationLifecycle::Finalizing;
    }
    return ExplorerOperationLifecycle::Queued;
}

} // namespace

@implementation ExplorerOperationProgressController {
    std::shared_ptr<nc::ops::Pool> m_Pool;
    nc::ops::Pool::ObservationTicket m_PoolObservation;
    __weak NCExplorerOperationProgressView *m_View;
    bool m_TickScheduled;
}

- (instancetype)initWithPool:(nc::ops::Pool &)_pool view:(NCExplorerOperationProgressView *)_view
{
    self = [super init];
    if( self ) {
        m_Pool = _pool.shared_from_this();
        m_View = _view;
        __weak ExplorerOperationProgressController *weak_self = self;
        m_PoolObservation = _pool.Observe(nc::ops::Pool::NotifyAboutChange, [weak_self] {
            dispatch_async(dispatch_get_main_queue(), ^{
              ExplorerOperationProgressController *const strong_self = weak_self;
              [strong_self poolDidChange];
            });
        });
        [self refresh];
        [self scheduleTickIfNeeded];
    }
    return self;
}

- (void)poolDidChange
{
    dispatch_assert_queue(dispatch_get_main_queue());
    [self refresh];
    [self scheduleTickIfNeeded];
}

- (void)scheduleTickIfNeeded
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( m_TickScheduled || !m_Pool || m_Pool->Empty() )
        return;
    m_TickScheduled = true;
    __weak ExplorerOperationProgressController *weak_self = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      ExplorerOperationProgressController *const strong_self = weak_self;
      if( !strong_self )
          return;
      strong_self->m_TickScheduled = false;
      [strong_self refresh];
      [strong_self scheduleTickIfNeeded];
    });
}

- (void)refresh
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !m_Pool )
        return;

    const auto operations = m_Pool->Operations();
    const auto finalizing = m_Pool->FinalizingOperations();
    std::unordered_set<const nc::ops::Operation *> finalizing_set;
    finalizing_set.reserve(finalizing.size());
    for( const auto &operation : finalizing )
        finalizing_set.emplace(operation.get());

    std::vector<ExplorerOperationProgressInput> inputs;
    inputs.reserve(operations.size());
    for( size_t index = 0; index != operations.size(); ++index ) {
        const std::shared_ptr<nc::ops::Operation> &operation = operations[index];
        if( !operation )
            continue;
        const nc::ops::Statistics &statistics = operation->Statistics();
        inputs.emplace_back(ExplorerOperationProgressInput{
            .stable_order = index,
            .title = operation->Title(),
            .current_item_path = operation->CurrentItemPath(),
            .lifecycle = Lifecycle(*operation, finalizing_set.contains(operation.get())),
            .preferred_unit = statistics.PreferredSource() == nc::ops::Statistics::SourceType::Bytes
                                  ? ExplorerOperationProgressUnit::Bytes
                                  : ExplorerOperationProgressUnit::Items,
            .bytes = Source(statistics, nc::ops::Statistics::SourceType::Bytes),
            .items = Source(statistics, nc::ops::Statistics::SourceType::Items),
        });
    }
    [m_View applySnapshot:ExplorerOperationProgressModel::Build(inputs)];
}

- (void)refreshNowForTesting
{
    [self refresh];
}

@end
