// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Pane/PaneSnapshot.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nc::core {

struct SearchRunId final {
    PaneId pane_id;
    uint64_t generation = 0;

    constexpr bool operator==(const SearchRunId &) const noexcept = default;
};

enum class SearchNameMatch : uint8_t {
    Contains,
    Exact
};

enum class SearchFileType : uint8_t {
    Any,
    RegularFile,
    Directory,
    SymbolicLink,
    Package,
    Other
};

enum class SearchScope : uint8_t {
    CurrentFolder,
    Recursive,
    CurrentDisk,
    SpotlightWholeMac
};

struct SearchSizeRange final {
    std::optional<uint64_t> minimum_bytes;
    std::optional<uint64_t> maximum_bytes;

    constexpr bool operator==(const SearchSizeRange &) const noexcept = default;
};

/** Inclusive Unix-time range used by the toolkit-independent search contract. */
struct SearchModifiedRange final {
    std::optional<int64_t> earliest_seconds;
    std::optional<int64_t> latest_seconds;

    constexpr bool operator==(const SearchModifiedRange &) const noexcept = default;
};

struct SearchFilters final {
    SearchNameMatch name_match = SearchNameMatch::Contains;
    std::optional<std::string> extension;
    SearchFileType file_type = SearchFileType::Any;
    SearchSizeRange size;
    SearchModifiedRange modified;
    std::optional<std::string> content;
    bool include_hidden = false;

    bool operator==(const SearchFilters &) const noexcept = default;
};

struct SearchRequest final {
    std::string query;
    SearchScope scope = SearchScope::CurrentFolder;
    SearchFilters filters;

    bool operator==(const SearchRequest &) const noexcept = default;
};

enum class SearchBackendKind : uint8_t {
    DirectScan,
    Spotlight
};

enum class SearchBackendSupport : uint8_t {
    Supported,
    Unsupported,
    Unavailable,
    IndexUnavailable
};

enum class SearchBackendLimitation : uint8_t {
    RecursiveScopeUnavailable,
    CurrentDiskScopeUnavailable,
    WholeMacScopeRequiresSpotlight,
    ContentSearchUnavailable,
    MetadataSearchUnavailable,
    HiddenItemsUnavailable,
    FullDiskAccessMissing,
    PermissionDeniedLocations,
    ResultPathsUnavailable,
    ProviderUnavailable,
    SpotlightUnavailable,
    SpotlightIndexUnavailable
};

struct SearchBackendCapabilities final {
    bool name = true;
    bool extension = true;
    bool type = false;
    bool size = false;
    bool modified = false;
    bool content = false;
    bool hidden_items = false;
    bool recursive_scope = false;
    bool current_disk_scope = false;
    bool whole_mac_scope = false;
    bool determinate_progress = false;

    constexpr bool operator==(const SearchBackendCapabilities &) const noexcept = default;
};

struct SearchBackendDescriptor final {
    SearchBackendKind kind = SearchBackendKind::DirectScan;
    SearchBackendSupport support = SearchBackendSupport::Unavailable;
    SearchBackendCapabilities capabilities;
    std::vector<SearchBackendLimitation> limitations;

    bool operator==(const SearchBackendDescriptor &) const noexcept = default;
};

struct SearchPlan final {
    SearchRequest request;
    SearchBackendDescriptor backend;
    /** Owned exact root for DirectScan; SpotlightWholeMac deliberately leaves it empty. */
    std::string execution_root;

    bool operator==(const SearchPlan &) const noexcept = default;
};

enum class SearchPhase : uint8_t {
    Idle,
    Preparing,
    Running,
    PartiallyCompleted,
    Completed,
    Cancelled,
    Failed,
    NoResults,
    TooManyResults,
    IndexUnavailable,
    BackendUnavailable,
    PermissionLimitedResults
};

struct SearchResultReference final {
    uint64_t count = 0;
    uint64_t generation = 0;
    /** Opaque controller-owned listing commit token. The pure store never retains a VFS listing. */
    std::string token;

    bool operator==(const SearchResultReference &) const noexcept = default;
};

enum class SearchFailureCode : uint8_t {
    ExecutionFailed,
    ProviderDisconnected,
    PermissionDenied,
    InvalidBackendReply
};

struct SearchFailure final {
    SearchFailureCode code = SearchFailureCode::ExecutionFailed;
    std::string detail;

    bool operator==(const SearchFailure &) const noexcept = default;
};

/**
 * Owned value projection of one pane's search mode.
 *
 * Snapshot() returns this value by copy. Backend/controller mutations therefore cannot alter a
 * previously observed snapshot, and no VFS object crosses this pure state boundary.
 */
struct SearchSnapshot final {
    PaneId pane_id;
    uint64_t revision = 0;
    std::optional<SearchRunId> run_id;
    SearchPhase phase = SearchPhase::Idle;
    std::optional<SearchRequest> request;
    std::optional<SearchBackendDescriptor> backend;
    std::optional<double> determinate_progress;
    std::optional<std::string> current_location;
    std::optional<uint64_t> scanned_count;
    std::optional<uint64_t> found_count;
    std::optional<SearchResultReference> results;
    std::optional<SearchFailure> failure;

    bool operator==(const SearchSnapshot &) const noexcept = default;
};

} // namespace nc::core
