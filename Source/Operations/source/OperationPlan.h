// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc::ops {

enum class OperationPlanType : uint8_t {
    Copy,
    Move,
    Rename,
    Trash,
    PermanentDelete
};

enum class OperationPlanDestinationKind : uint8_t {
    Directory,
    ExactItem
};

enum class OperationPlanConflictDecision : uint8_t {
    Ask,
    Replace,
    Skip,
    KeepBoth,
    RenameNew,
    RenameExisting,
    MergeFolders
};

enum class OperationPlanConflictScope : uint8_t {
    ThisItem,
    AllItems,
    SameExtension,
    SameFolder
};

enum class OperationPlanValidationError : uint8_t {
    InvalidPlanId,
    InvalidType,
    EmptySources,
    InvalidSourceProviderId,
    InvalidSourcePath,
    DuplicateSource,
    MissingDestination,
    UnexpectedDestination,
    InvalidDestinationProviderId,
    InvalidDestinationPath,
    InvalidDestinationKind,
    ExactDestinationRequiresSingleSource,
    RenameRequiresSingleSource,
    RenameRequiresExactDestination,
    RenameRequiresSameProvider,
    MissingConflictPolicy,
    UnexpectedConflictPolicy,
    MissingCreatedAt,
    InvalidConflictDecision,
    InvalidConflictScope
};

struct OperationPlanSourceInput final {
    std::string provider_id;
    std::string absolute_path;
};

struct OperationPlanDestinationInput final {
    OperationPlanDestinationInput() = delete;
    OperationPlanDestinationInput(std::string _provider_id,
                                  std::string _absolute_path,
                                  OperationPlanDestinationKind _kind)
        : provider_id(std::move(_provider_id)), absolute_path(std::move(_absolute_path)), kind(_kind)
    {
    }

    std::string provider_id;
    std::string absolute_path;
    OperationPlanDestinationKind kind;
};

class OperationPlanConflictPolicy final
{
public:
    OperationPlanConflictPolicy() = delete;
    constexpr OperationPlanConflictPolicy(OperationPlanConflictDecision _decision,
                                          OperationPlanConflictScope _scope) noexcept
        : m_Decision(_decision), m_Scope(_scope)
    {
    }

    [[nodiscard]] constexpr OperationPlanConflictDecision Decision() const noexcept { return m_Decision; }
    [[nodiscard]] constexpr OperationPlanConflictScope Scope() const noexcept { return m_Scope; }

    bool operator==(const OperationPlanConflictPolicy &) const = default;

private:
    OperationPlanConflictDecision m_Decision;
    OperationPlanConflictScope m_Scope;
};

struct OperationPlanInput final {
    std::string plan_id;
    const OperationPlanType type = static_cast<OperationPlanType>(255);
    std::vector<OperationPlanSourceInput> sources;
    std::optional<OperationPlanDestinationInput> destination;
    std::optional<OperationPlanConflictPolicy> conflict_policy;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

class OperationPlanId final
{
public:
    OperationPlanId() = delete;

    [[nodiscard]] std::string_view Value() const noexcept { return m_Value; }
    bool operator==(const OperationPlanId &) const = default;

private:
    explicit OperationPlanId(std::string _value) : m_Value(std::move(_value)) {}

    std::string m_Value;

    friend class OperationPlan;
};

class OperationProviderId final
{
public:
    OperationProviderId() = delete;

    [[nodiscard]] std::string_view Value() const noexcept { return m_Value; }
    bool operator==(const OperationProviderId &) const = default;

private:
    explicit OperationProviderId(std::string _value) : m_Value(std::move(_value)) {}

    std::string m_Value;

    friend class OperationPlan;
};

class OperationPlanSource final
{
public:
    OperationPlanSource() = delete;

    [[nodiscard]] const OperationProviderId &ProviderId() const noexcept { return m_ProviderId; }
    [[nodiscard]] std::string_view AbsolutePath() const noexcept { return m_AbsolutePath; }
    bool operator==(const OperationPlanSource &) const = default;

private:
    OperationPlanSource(OperationProviderId _provider_id, std::string _absolute_path)
        : m_ProviderId(std::move(_provider_id)), m_AbsolutePath(std::move(_absolute_path))
    {
    }

    OperationProviderId m_ProviderId;
    std::string m_AbsolutePath;

    friend class OperationPlan;
};

class OperationPlanDestination final
{
public:
    OperationPlanDestination() = delete;

    [[nodiscard]] const OperationProviderId &ProviderId() const noexcept { return m_ProviderId; }
    [[nodiscard]] std::string_view AbsolutePath() const noexcept { return m_AbsolutePath; }
    [[nodiscard]] OperationPlanDestinationKind Kind() const noexcept { return m_Kind; }
    bool operator==(const OperationPlanDestination &) const = default;

private:
    OperationPlanDestination(OperationProviderId _provider_id,
                             std::string _absolute_path,
                             OperationPlanDestinationKind _kind)
        : m_ProviderId(std::move(_provider_id)), m_AbsolutePath(std::move(_absolute_path)), m_Kind(_kind)
    {
    }

    OperationProviderId m_ProviderId;
    std::string m_AbsolutePath;
    OperationPlanDestinationKind m_Kind;

    friend class OperationPlan;
};

enum class OperationPlanSourceEffect : uint8_t {
    Unchanged,
    Relocated,
    Deleted
};

enum class OperationPlanDestinationEffect : uint8_t {
    None,
    CreateOrUpdate
};

enum class OperationPlanDataLossRisk : uint8_t {
    None,
    Recoverable,
    Irreversible
};

/** Effects that are unavoidable for an operation type before provider validation or preflight. */
class OperationPlanIntrinsicEffects final
{
public:
    OperationPlanIntrinsicEffects() = delete;

    [[nodiscard]] OperationPlanSourceEffect Source() const noexcept { return m_Source; }
    [[nodiscard]] OperationPlanDestinationEffect Destination() const noexcept { return m_Destination; }
    [[nodiscard]] OperationPlanDataLossRisk DataLossRisk() const noexcept { return m_DataLossRisk; }
    bool operator==(const OperationPlanIntrinsicEffects &) const = default;

private:
    constexpr OperationPlanIntrinsicEffects(OperationPlanSourceEffect _source,
                                            OperationPlanDestinationEffect _destination,
                                            OperationPlanDataLossRisk _data_loss_risk) noexcept
        : m_Source(_source), m_Destination(_destination), m_DataLossRisk(_data_loss_risk)
    {
    }

    OperationPlanSourceEffect m_Source;
    OperationPlanDestinationEffect m_Destination;
    OperationPlanDataLossRisk m_DataLossRisk;

    friend class OperationPlan;
};

/**
 * Immutable value describing structurally valid filesystem mutation intent.
 *
 * Creation does not perform preflight, grant approval, bind a provider, or make the plan executable.
 * Those are later lifecycle stages owned by the operation planner and engine adapters.
 */
class OperationPlan final
{
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    OperationPlan() = delete;

    [[nodiscard]] static std::expected<OperationPlan, OperationPlanValidationError>
    Create(OperationPlanInput _input);

    [[nodiscard]] const OperationPlanId &Id() const noexcept { return m_Id; }
    [[nodiscard]] OperationPlanType Type() const noexcept { return m_Type; }
    [[nodiscard]] const std::vector<OperationPlanSource> &Sources() const noexcept { return m_Sources; }
    [[nodiscard]] const std::optional<OperationPlanDestination> &Destination() const noexcept
    {
        return m_Destination;
    }
    [[nodiscard]] const std::optional<OperationPlanConflictPolicy> &ConflictPolicy() const noexcept
    {
        return m_ConflictPolicy;
    }
    [[nodiscard]] TimePoint CreatedAt() const noexcept { return m_CreatedAt; }
    /**
     * Returns only unavoidable type-derived effects. Conflict resolution and preflight effects belong to a later
     * accepted-plan report and are intentionally absent from this structural value.
     */
    [[nodiscard]] const OperationPlanIntrinsicEffects &IntrinsicEffects() const noexcept { return m_IntrinsicEffects; }

    bool operator==(const OperationPlan &) const = default;

private:
    [[nodiscard]] static OperationPlanIntrinsicEffects DeriveIntrinsicEffects(OperationPlanType _type) noexcept;

    OperationPlan(OperationPlanId _id,
                  OperationPlanType _type,
                  std::vector<OperationPlanSource> _sources,
                  std::optional<OperationPlanDestination> _destination,
                  std::optional<OperationPlanConflictPolicy> _conflict_policy,
                  TimePoint _created_at,
                  OperationPlanIntrinsicEffects _intrinsic_effects);

    OperationPlanId m_Id;
    OperationPlanType m_Type;
    std::vector<OperationPlanSource> m_Sources;
    std::optional<OperationPlanDestination> m_Destination;
    std::optional<OperationPlanConflictPolicy> m_ConflictPolicy;
    TimePoint m_CreatedAt;
    OperationPlanIntrinsicEffects m_IntrinsicEffects;
};

} // namespace nc::ops
