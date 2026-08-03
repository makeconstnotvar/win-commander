// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationJournal.h"

#include <functional>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>

namespace nc::ops {

struct OperationJournalSyscalls final {
    std::function<int(const char *, int, mode_t)> open;
    std::function<int(int, const char *, int, mode_t)> open_at;
    std::function<ssize_t(int, void *, size_t)> read;
    std::function<ssize_t(int, const void *, size_t)> write;
    std::function<int(int)> fsync;
    std::function<int(int, struct stat *)> fstat;
    std::function<int(int, int)> flock;
    std::function<int(int, const char *, int, const char *)> rename_at;
    std::function<int(int)> close;
    std::function<void(void *, size_t)> random_bytes;
};

class OperationJournalTesting final
{
public:
    using Clock = std::function<OperationPlan::TimePoint()>;

    [[nodiscard]] static std::shared_ptr<OperationJournalSyscalls> DefaultSyscalls();
    [[nodiscard]] static std::expected<OperationJournal, OperationJournalError>
    Open(std::string_view _absolute_existing_parent,
         std::shared_ptr<OperationJournalSyscalls> _syscalls,
         Clock _clock);

    [[nodiscard]] static OperationJournalAdmissionReceipt
    ForgeAdmissionReceipt(const OperationJournal &_journal, OperationId _operation_id, OperationPlan _plan);

    [[nodiscard]] static OperationJournalRunReceipt
    ForgeRunReceipt(const OperationJournal &_journal, OperationId _operation_id, OperationPlan _plan);

    /** Test-only fixture path for decoding/migration cases that require a chosen persisted ID. */
    [[nodiscard]] static std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
    AdmitWithOperationId(OperationJournal &_journal, OperationId _operation_id, const OperationPlan &_plan);

    [[nodiscard]] static std::expected<void, OperationJournalError>
    Transition(OperationJournal &_journal, std::string_view _plan_id, OperationJournalState _state);

    [[nodiscard]] static std::expected<void, OperationJournalError>
    RecordItemResult(OperationJournal &_journal,
                     std::string_view _plan_id,
                     OperationJournalItemResult _result);
};

} // namespace nc::ops
