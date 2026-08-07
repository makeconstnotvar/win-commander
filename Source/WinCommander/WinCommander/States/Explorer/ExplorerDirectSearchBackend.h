// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFSListing.h>
#include <WinCommander/Core/Search/SearchState.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nc::explorer {

struct ExplorerSearchBackendLimits final {
    size_t maximum_results = 50'000;
    size_t maximum_path_bytes = 64 * 1024 * 1024;
};

struct ExplorerSearchBackendInput final {
    core::SearchPlan plan;
    VFSHostPtr origin_host;
    unsigned long fetch_flags = 0;
    ExplorerSearchBackendLimits limits;
};

struct ExplorerSearchBackendProgress final {
    std::optional<double> determinate_progress;
    std::optional<std::string> current_location;
    std::optional<uint64_t> scanned_count;
    std::optional<uint64_t> found_count;
};

enum class ExplorerSearchBackendCompletionKind : uint8_t {
    Completed,
    Partial,
    PermissionLimited,
    TooManyResults,
    IndexUnavailable,
    BackendUnavailable,
    Cancelled,
    Failed
};

struct ExplorerSearchBackendCompletion final {
    ExplorerSearchBackendCompletionKind kind = ExplorerSearchBackendCompletionKind::Failed;
    VFSListingPtr listing;
    uint64_t accepted_count = 0;
    std::vector<core::SearchBackendLimitation> limitations;
    std::optional<core::SearchFailure> failure;
};

/**
 * One accepted backend execution. Stop is non-blocking; Wait may block and must run off main.
 * The controller keeps the run alive until a background reaper has returned from Wait().
 */
class ExplorerSearchBackendRun
{
public:
    virtual ~ExplorerSearchBackendRun() = default;
    virtual void Stop() noexcept = 0;
    virtual void Wait() noexcept = 0;
};

/** Backend-neutral async seam. A future Spotlight adapter implements the same value callbacks. */
class ExplorerSearchBackend
{
public:
    using ProgressCallback = std::function<void(ExplorerSearchBackendProgress)>;
    using CompletionCallback = std::function<void(ExplorerSearchBackendCompletion)>;

    virtual ~ExplorerSearchBackend() = default;

    /**
     * Returns one owning run when execution was admitted. Callbacks may arrive from any queue;
     * exactly one completion is delivered for an admitted run.
     */
    [[nodiscard]] virtual std::shared_ptr<ExplorerSearchBackendRun>
    Start(ExplorerSearchBackendInput _input, ProgressCallback _progress, CompletionCallback _completion) = 0;
};

/** Direct provider scan implemented through nc::vfs::SearchForFiles. */
class ExplorerDirectSearchBackend final : public ExplorerSearchBackend
{
public:
    [[nodiscard]] std::shared_ptr<ExplorerSearchBackendRun>
    Start(ExplorerSearchBackendInput _input, ProgressCallback _progress, CompletionCallback _completion) override;
};

} // namespace nc::explorer
