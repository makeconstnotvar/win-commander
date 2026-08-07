// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nc::panel {

enum class DragDropOperation : uint8_t {
    Forbidden,
    Copy,
    Move,
    Link
};

enum class DragDropFailure : uint8_t {
    None,
    NoSources,
    InvalidModifierIntent,
    UnknownProviderIdentity,
    UnknownVolumeIdentity,
    InconsistentProviderFacts,
    InvalidPath,
    DuplicateSource,
    SameFolder,
    RecursiveDestination,
    SourceUnreadable,
    SourceReadOnly,
    SourceMutationUnsupported,
    DestinationReadOnly,
    DestinationUnsupported,
    LinkUnsupported,
    InternalFailure
};

enum class DragDropItemKind : uint8_t {
    RegularFile,
    Directory,
    SymbolicLink,
    Other
};

enum class DragDropModifierIntent : uint8_t {
    Automatic,
    Copy,
    Move,
    Link,
    Invalid
};

/**
 * A normalized projection of the live modifier/source-operation state. The AppKit adapter owns the
 * conversion from NSEvent/NSDragOperation flags; this pure policy accepts only semantic requests.
 */
struct DragDropModifierState final {
    bool force_copy{false};
    bool force_move{false};
    bool force_link{false};

    bool operator==(const DragDropModifierState &) const noexcept = default;
};

/** Path-scoped ProviderCapabilitiesResolver evidence captured at the exact source/destination directory. */
struct DragDropPathCapabilities final {
    bool can_read{false};
    bool can_write{false};
    bool can_create_file{false};
    bool can_create_folder{false};
    bool can_create_symlink{false};
    bool can_rename{false};
    bool can_delete_permanently{false};

    bool operator==(const DragDropPathCapabilities &) const noexcept = default;
};

/**
 * Exact, synchronous source evidence. Identity values are borrowed pointer identities converted to
 * uintptr_t by the integration layer and are valid only for the lifetime of one drag decision.
 */
struct DragDropSourceFacts final {
    uintptr_t provider_identity{0};
    uintptr_t volume_identity{0};
    bool native_namespace{false};
    DragDropItemKind kind{DragDropItemKind::Other};
    std::string path;
    std::string directory;
    DragDropPathCapabilities capabilities;

    bool operator==(const DragDropSourceFacts &) const noexcept = default;
};

/** Exact destination evidence captured for the directory currently under the pointer. */
struct DragDropDestinationFacts final {
    uintptr_t provider_identity{0};
    uintptr_t volume_identity{0};
    bool native_namespace{false};
    std::string directory;
    DragDropPathCapabilities capabilities;

    bool operator==(const DragDropDestinationFacts &) const noexcept = default;
};

struct DragDropPolicyInput final {
    DragDropModifierState modifiers;
    std::vector<DragDropSourceFacts> sources;
    DragDropDestinationFacts destination;

    bool operator==(const DragDropPolicyInput &) const noexcept = default;
};

/**
 * An immutable decision carrying the exact facts used to produce its badge. Receive must either use
 * these facts or rebuild the decision from a fresh live snapshot before enqueueing an operation.
 */
class DragDropPolicyDecision final
{
public:
    [[nodiscard]] DragDropOperation Operation() const noexcept { return m_Operation; }
    [[nodiscard]] DragDropFailure Failure() const noexcept { return m_Failure; }
    [[nodiscard]] bool Allowed() const noexcept { return m_Operation != DragDropOperation::Forbidden; }
    [[nodiscard]] const DragDropPolicyInput &Input() const noexcept { return m_Input; }

private:
    DragDropPolicyDecision(DragDropOperation _operation, DragDropFailure _failure, DragDropPolicyInput _input) noexcept;

    DragDropOperation m_Operation{DragDropOperation::Forbidden};
    DragDropFailure m_Failure{DragDropFailure::InternalFailure};
    DragDropPolicyInput m_Input;

    friend DragDropPolicyDecision EvaluateDragDropPolicy(DragDropPolicyInput) noexcept;
};

[[nodiscard]] DragDropModifierIntent MapDragDropModifiers(const DragDropModifierState &_state) noexcept;

/** Deterministic, side-effect-free drag/drop operation selection. */
[[nodiscard]] DragDropPolicyDecision EvaluateDragDropPolicy(DragDropPolicyInput _input) noexcept;

} // namespace nc::panel
