// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "SearchState.h"

#include <expected>
#include <optional>
#include <string>

namespace nc::core {

enum class SearchPlanningFailure : uint8_t {
    EmptyCriteria,
    InvalidExtension,
    InvalidSizeRange,
    InvalidModifiedRange,
    MissingCurrentFolder,
    MissingCurrentDiskRoot
};

/** Runtime facts normalized at the controller/provider boundary before backend selection. */
struct SearchPlanningFacts final {
    std::string current_folder;
    std::optional<std::string> current_disk_root;
    bool provider_available = true;
    bool provider_is_native = false;
    bool provider_supports_recursive = false;
    bool provider_supports_current_disk = false;
    bool provider_supports_metadata = false;
    bool provider_supports_content = false;
    bool provider_supports_hidden_items = false;
    bool provider_reports_determinate_progress = false;
    bool spotlight_available = false;
    bool spotlight_index_available = false;
    bool spotlight_supports_content = true;
    bool full_disk_access = false;

    bool operator==(const SearchPlanningFacts &) const noexcept = default;
};

class SearchPlanning final
{
public:
    using Result = std::expected<SearchPlan, SearchPlanningFailure>;

    /** Trims textual criteria, canonicalizes an extension and validates range ordering. */
    [[nodiscard]] static std::expected<SearchRequest, SearchPlanningFailure> Normalize(SearchRequest _request);

    /** Selects a direct or Spotlight backend and exposes every blocking/non-blocking limitation. */
    [[nodiscard]] static Result Plan(SearchRequest _request, SearchPlanningFacts _facts);

    /** Defensive validation for a plan crossing into SearchStore. */
    [[nodiscard]] static bool IsValid(const SearchPlan &_plan);
};

} // namespace nc::core
