// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationPlan.h"

#include <algorithm>
#include <set>
#include <utility>

namespace nc::ops {
namespace {

bool IsValid(OperationPlanType _type) noexcept
{
    switch( _type ) {
        case OperationPlanType::Copy:
        case OperationPlanType::Move:
        case OperationPlanType::Rename:
        case OperationPlanType::Trash:
        case OperationPlanType::PermanentDelete:
            return true;
    }
    return false;
}

bool IsValid(OperationPlanDestinationKind _kind) noexcept
{
    switch( _kind ) {
        case OperationPlanDestinationKind::Directory:
        case OperationPlanDestinationKind::ExactItem:
            return true;
    }
    return false;
}

bool IsValid(OperationPlanConflictDecision _decision) noexcept
{
    switch( _decision ) {
        case OperationPlanConflictDecision::Ask:
        case OperationPlanConflictDecision::Replace:
        case OperationPlanConflictDecision::Skip:
        case OperationPlanConflictDecision::KeepBoth:
        case OperationPlanConflictDecision::RenameNew:
        case OperationPlanConflictDecision::RenameExisting:
        case OperationPlanConflictDecision::MergeFolders:
            return true;
    }
    return false;
}

bool IsValid(OperationPlanConflictScope _scope) noexcept
{
    switch( _scope ) {
        case OperationPlanConflictScope::ThisItem:
        case OperationPlanConflictScope::AllItems:
        case OperationPlanConflictScope::SameExtension:
        case OperationPlanConflictScope::SameFolder:
            return true;
    }
    return false;
}

bool IsValidAbsolutePath(std::string_view _path) noexcept
{
    return !_path.empty() && _path.front() == '/' &&
           std::ranges::find(_path, '\0') == _path.end();
}

bool IsValidIdentity(std::string_view _identity) noexcept
{
    return !_identity.empty() && std::ranges::find(_identity, '\0') == _identity.end();
}

} // namespace

std::expected<OperationPlan, OperationPlanValidationError>
OperationPlan::Create(OperationPlanInput _input)
{
    if( !IsValidIdentity(_input.plan_id) )
        return std::unexpected(OperationPlanValidationError::InvalidPlanId);
    if( !IsValid(_input.type) )
        return std::unexpected(OperationPlanValidationError::InvalidType);
    if( !_input.created_at )
        return std::unexpected(OperationPlanValidationError::MissingCreatedAt);
    if( _input.sources.empty() )
        return std::unexpected(OperationPlanValidationError::EmptySources);
    if( _input.conflict_policy ) {
        if( !IsValid(_input.conflict_policy->Decision()) )
            return std::unexpected(OperationPlanValidationError::InvalidConflictDecision);
        if( !IsValid(_input.conflict_policy->Scope()) )
            return std::unexpected(OperationPlanValidationError::InvalidConflictScope);
    }

    std::set<std::pair<std::string_view, std::string_view>> identities;
    for( const auto &source : _input.sources ) {
        if( !IsValidIdentity(source.provider_id) )
            return std::unexpected(OperationPlanValidationError::InvalidSourceProviderId);
        if( !IsValidAbsolutePath(source.absolute_path) )
            return std::unexpected(OperationPlanValidationError::InvalidSourcePath);
        if( !identities.emplace(source.provider_id, source.absolute_path).second )
            return std::unexpected(OperationPlanValidationError::DuplicateSource);
    }

    if( _input.destination ) {
        if( !IsValidIdentity(_input.destination->provider_id) )
            return std::unexpected(OperationPlanValidationError::InvalidDestinationProviderId);
        if( !IsValidAbsolutePath(_input.destination->absolute_path) )
            return std::unexpected(OperationPlanValidationError::InvalidDestinationPath);
        if( !IsValid(_input.destination->kind) )
            return std::unexpected(OperationPlanValidationError::InvalidDestinationKind);
        if( _input.destination->kind == OperationPlanDestinationKind::ExactItem &&
            (_input.destination->absolute_path == "/" || _input.destination->absolute_path.back() == '/') )
            return std::unexpected(OperationPlanValidationError::InvalidDestinationPath);
    }

    switch( _input.type ) {
        case OperationPlanType::Copy:
        case OperationPlanType::Move:
            if( !_input.conflict_policy )
                return std::unexpected(OperationPlanValidationError::MissingConflictPolicy);
            if( !_input.destination )
                return std::unexpected(OperationPlanValidationError::MissingDestination);
            if( _input.destination->kind == OperationPlanDestinationKind::ExactItem && _input.sources.size() != 1 )
                return std::unexpected(OperationPlanValidationError::ExactDestinationRequiresSingleSource);
            break;
        case OperationPlanType::Rename:
            if( !_input.conflict_policy )
                return std::unexpected(OperationPlanValidationError::MissingConflictPolicy);
            if( _input.sources.size() != 1 )
                return std::unexpected(OperationPlanValidationError::RenameRequiresSingleSource);
            if( !_input.destination )
                return std::unexpected(OperationPlanValidationError::MissingDestination);
            if( _input.destination->kind != OperationPlanDestinationKind::ExactItem )
                return std::unexpected(OperationPlanValidationError::RenameRequiresExactDestination);
            if( _input.destination->provider_id != _input.sources.front().provider_id )
                return std::unexpected(OperationPlanValidationError::RenameRequiresSameProvider);
            break;
        case OperationPlanType::Trash:
        case OperationPlanType::PermanentDelete:
            if( _input.conflict_policy )
                return std::unexpected(OperationPlanValidationError::UnexpectedConflictPolicy);
            if( _input.destination )
                return std::unexpected(OperationPlanValidationError::UnexpectedDestination);
            break;
    }

    std::vector<OperationPlanSource> sources;
    sources.reserve(_input.sources.size());
    for( auto &source : _input.sources ) {
        sources.push_back(
            OperationPlanSource{OperationProviderId{std::move(source.provider_id)}, std::move(source.absolute_path)});
    }

    std::optional<OperationPlanDestination> destination;
    if( _input.destination ) {
        destination = OperationPlanDestination{OperationProviderId{std::move(_input.destination->provider_id)},
                                               std::move(_input.destination->absolute_path),
                                               _input.destination->kind};
    }

    const auto intrinsic_effects = DeriveIntrinsicEffects(_input.type);
    return OperationPlan{OperationPlanId{std::move(_input.plan_id)},
                         _input.type,
                         std::move(sources),
                         std::move(destination),
                         _input.conflict_policy,
                         *_input.created_at,
                         intrinsic_effects};
}

OperationPlanIntrinsicEffects OperationPlan::DeriveIntrinsicEffects(OperationPlanType _type) noexcept
{
    switch( _type ) {
        case OperationPlanType::Copy:
            return {OperationPlanSourceEffect::Unchanged,
                    OperationPlanDestinationEffect::CreateOrUpdate,
                    OperationPlanDataLossRisk::None};
        case OperationPlanType::Move:
        case OperationPlanType::Rename:
            return {OperationPlanSourceEffect::Relocated,
                    OperationPlanDestinationEffect::CreateOrUpdate,
                    OperationPlanDataLossRisk::None};
        case OperationPlanType::Trash:
            return {OperationPlanSourceEffect::Relocated,
                    OperationPlanDestinationEffect::None,
                    OperationPlanDataLossRisk::Recoverable};
        case OperationPlanType::PermanentDelete:
            return {OperationPlanSourceEffect::Deleted,
                    OperationPlanDestinationEffect::None,
                    OperationPlanDataLossRisk::Irreversible};
    }
    std::unreachable();
}

OperationPlan::OperationPlan(OperationPlanId _id,
                             OperationPlanType _type,
                             std::vector<OperationPlanSource> _sources,
                             std::optional<OperationPlanDestination> _destination,
                             std::optional<OperationPlanConflictPolicy> _conflict_policy,
                             TimePoint _created_at,
                             OperationPlanIntrinsicEffects _intrinsic_effects)
    : m_Id(std::move(_id)), m_Type(_type), m_Sources(std::move(_sources)), m_Destination(std::move(_destination)),
      m_ConflictPolicy(_conflict_policy), m_CreatedAt(_created_at), m_IntrinsicEffects(_intrinsic_effects)
{
}

} // namespace nc::ops
