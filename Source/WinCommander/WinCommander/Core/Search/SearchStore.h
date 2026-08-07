// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "SearchPlanning.h"

#include <expected>
#include <limits>

namespace nc::core {

enum class SearchStoreFailure : uint8_t {
    ZeroPaneId,
    InvalidPlan,
    RunIdExhausted,
    NoActiveRun,
    StaleRun,
    TerminalRun,
    InvalidTransition,
    InvalidProgress,
    InvalidResults,
    MissingResults,
    LimitationMismatch,
    InvalidLimitation
};

struct SearchProgressUpdate final {
    std::optional<double> determinate_progress;
    std::optional<std::string> current_location;
    std::optional<uint64_t> scanned_count;
    std::optional<uint64_t> found_count;
};

enum class SearchCompletionKind : uint8_t {
    Complete,
    Partial,
    TooManyResults,
    PermissionLimited
};

using SearchStoreMutationResult = std::expected<void, SearchStoreFailure>;

/**
 * Main-queue-agnostic pure reducer for one pane's Search Mode.
 *
 * External synchronization belongs to the controller. Every accepted event advances revision;
 * rejected events are atomic. Run identities are pane-bound and never reused by this instance.
 */
class SearchStore final
{
public:
    using CreateResult = std::expected<SearchStore, SearchStoreFailure>;
    using StartResult = std::expected<SearchRunId, SearchStoreFailure>;

    [[nodiscard]] static CreateResult Create(PaneId _pane_id) noexcept;

    [[nodiscard]] SearchSnapshot Snapshot() const { return m_State; }
    [[nodiscard]] StartResult Start(const SearchPlan &_plan);
    [[nodiscard]] SearchStoreMutationResult MarkRunning(SearchRunId _run_id);
    [[nodiscard]] SearchStoreMutationResult UpdateProgress(SearchRunId _run_id, SearchProgressUpdate _update);
    /** Publishes a typed limitation discovered after a valid backend has started. */
    [[nodiscard]] SearchStoreMutationResult ReportLimitation(SearchRunId _run_id, SearchBackendLimitation _limitation);
    [[nodiscard]] SearchStoreMutationResult PublishResults(SearchRunId _run_id, SearchResultReference _results);
    [[nodiscard]] SearchStoreMutationResult Complete(SearchRunId _run_id, SearchCompletionKind _kind);
    [[nodiscard]] SearchStoreMutationResult Cancel(SearchRunId _run_id);
    [[nodiscard]] SearchStoreMutationResult Fail(SearchRunId _run_id, SearchFailure _failure);
    void Reset() noexcept;

private:
    explicit SearchStore(PaneId _pane_id) noexcept;

    [[nodiscard]] SearchStoreMutationResult ValidateActive(SearchRunId _run_id) const noexcept;
    [[nodiscard]] bool IsActivePhase() const noexcept;
    void CommitTerminal(SearchPhase _phase) noexcept;

    SearchSnapshot m_State;
    uint64_t m_LastRunGeneration = 0;
};

} // namespace nc::core
