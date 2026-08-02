// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationJournal.h"

#include <VFS/ProviderCapabilities.h>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace nc::ops {

struct ProviderConditionalCopyJournalContext final {
    size_t item_index{0};
    uint64_t exact_source_bytes{0};

    bool operator==(const ProviderConditionalCopyJournalContext &) const noexcept = default;
};

enum class ProviderConditionalCopyJournalMappingError : uint8_t {
    NonExecutionTerminal,
    InconsistentResult
};

/**
 * Converts one provider terminal result into the journal schema without granting execution authority.
 * Aborted is a valid provider terminal state, but it precedes execution and therefore has no item result.
 */
[[nodiscard]] std::expected<OperationJournalItemResult, ProviderConditionalCopyJournalMappingError>
MapProviderConditionalCopyCommitResultToJournalItemResult(
    const vfs::ProviderConditionalCopyCommitResult &_result,
    ProviderConditionalCopyJournalContext _context) noexcept;

} // namespace nc::ops
