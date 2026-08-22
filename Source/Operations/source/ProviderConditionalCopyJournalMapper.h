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
    /**
     * Which axis this item's commit result belongs on. A Delete removes a source instead of
     * publishing a destination, so its `Published` outcome has to land on the journal's
     * `source_removal` field rather than `destination_publication` - the two axes are never both
     * active for one item, the same partition `OperationJournalRemovesSource` already draws by plan
     * type, restated here because this context is what the mapper actually sees.
     */
    bool removes_source{false};

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
