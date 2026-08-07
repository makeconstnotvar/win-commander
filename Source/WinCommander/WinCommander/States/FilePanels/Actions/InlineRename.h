// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nc::ops {
class Copying;
}

namespace nc::panel::actions {

enum class InlineRenameStatus : uint8_t {
    Ready,
    Unchanged,
    InvalidName,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    ListingUnavailable,
    StaleSource,
    DestinationReadOnly,
    ProviderUnsupported,
    CaseSensitivityUnavailable,
    DestinationExists,
    UnsafeCaseOnlyRename
};

/** One synchronous pane snapshot supplied by the controller adapter. */
struct InlineRenameLiveContext final {
    bool pane_available{false};
    bool window_available{false};
    bool loading{false};
    bool listing_loaded{false};
    bool uniform{false};
    VFSListingPtr listing;
    unsigned long generation{0};
    VFSHostPtr host;
    std::string directory;
};

struct InlineRenamePlanningResult;

class InlineRenamePlan final
{
public:
    InlineRenamePlan(const InlineRenamePlan &) = default;
    InlineRenamePlan(InlineRenamePlan &&) noexcept = default;
    InlineRenamePlan &operator=(const InlineRenamePlan &) = default;
    InlineRenamePlan &operator=(InlineRenamePlan &&) noexcept = default;
    ~InlineRenamePlan() = default;

    [[nodiscard]] const VFSListingItem &Source() const noexcept { return m_Source; }
    [[nodiscard]] const VFSListingPtr &Listing() const noexcept { return m_Listing; }
    [[nodiscard]] unsigned long Generation() const noexcept { return m_Generation; }
    [[nodiscard]] const VFSHostPtr &Host() const noexcept { return m_Host; }
    [[nodiscard]] const std::string &Directory() const noexcept { return m_Directory; }
    [[nodiscard]] const std::string &DestinationName() const noexcept { return m_DestinationName; }
    [[nodiscard]] const std::string &DestinationPath() const noexcept { return m_DestinationPath; }
    [[nodiscard]] bool CaseSensitive() const noexcept { return m_CaseSensitive; }
    [[nodiscard]] bool IsCaseOnlyRename() const noexcept { return m_CaseOnlyRename; }

private:
    InlineRenamePlan(VFSListingItem _source,
                     VFSListingPtr _listing,
                     unsigned long _generation,
                     VFSHostPtr _host,
                     std::string _directory,
                     std::string _destination_name,
                     std::string _destination_path,
                     bool _case_sensitive,
                     bool _case_only_rename) noexcept;

    VFSListingItem m_Source;
    VFSListingPtr m_Listing;
    unsigned long m_Generation{0};
    VFSHostPtr m_Host;
    std::string m_Directory;
    std::string m_DestinationName;
    std::string m_DestinationPath;
    bool m_CaseSensitive{true};
    bool m_CaseOnlyRename{false};

    friend InlineRenamePlanningResult PlanInlineRename(const InlineRenameLiveContext &,
                                                        const VFSListingItem &,
                                                        std::string_view) noexcept;
};

struct InlineRenamePlanningResult final {
    InlineRenameStatus status{InlineRenameStatus::ListingUnavailable};
    std::optional<InlineRenamePlan> plan;
};

/** Pure synchronous admission against one exact live pane snapshot. */
[[nodiscard]] InlineRenamePlanningResult PlanInlineRename(const InlineRenameLiveContext &_live,
                                                          const VFSListingItem &_source,
                                                          std::string_view _destination_name) noexcept;

/** Provider-only worker check; it captures no PanelController or window object. */
[[nodiscard]] bool RevalidateInlineRenameRuntime(const InlineRenamePlan &_plan) noexcept;

/** Constructs the established legacy move operation and installs the provider-only runtime preflight. */
[[nodiscard]] std::shared_ptr<nc::ops::Copying>
MakeInlineRenameOperation(const InlineRenamePlan &_plan) noexcept;

} // namespace nc::panel::actions
