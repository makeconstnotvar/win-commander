// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationPlanner.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace nc::ops {
namespace {

std::string Key(const OperationPlanningPath &_path)
{
    std::string key = _path.provider_id;
    key.push_back('\0');
    key += _path.absolute_path;
    return key;
}

std::string AccessKey(const OperationPlanningPath &_path, OperationPlanningRequiredAccess _required)
{
    auto key = Key(_path);
    key.push_back(static_cast<char>(_required));
    return key;
}

std::string NormalizeAbsolutePath(std::string_view _path)
{
    std::vector<std::string_view> components;
    size_t position = 0;
    while( position < _path.size() ) {
        while( position < _path.size() && _path[position] == '/' )
            ++position;
        const auto end = _path.find('/', position);
        const auto component = _path.substr(position, end == std::string_view::npos ? _path.size() - position
                                                                                   : end - position);
        position = end == std::string_view::npos ? _path.size() : end;
        if( component.empty() || component == "." )
            continue;
        if( component == ".." ) {
            if( !components.empty() )
                components.pop_back();
            continue;
        }
        components.push_back(component);
    }

    std::string normalized = "/";
    for( size_t index = 0; index < components.size(); ++index ) {
        if( index != 0 )
            normalized.push_back('/');
        normalized.append(components[index]);
    }
    return normalized;
}

std::string ItemKey(const OperationPlanningPath &_path)
{
    std::string key = _path.provider_id;
    key.push_back('\0');
    key += NormalizeAbsolutePath(_path.absolute_path);
    return key;
}

std::string TrimTrailingSlashes(std::string_view _path)
{
    auto end = _path.size();
    while( end > 1 && _path[end - 1] == '/' )
        --end;
    return std::string{_path.substr(0, end)};
}

std::string ParentPath(std::string_view _path)
{
    const auto trimmed = TrimTrailingSlashes(_path);
    if( trimmed == "/" )
        return trimmed;
    const auto separator = trimmed.rfind('/');
    return separator == 0 ? std::string{"/"} : trimmed.substr(0, separator);
}

std::string BaseName(std::string_view _path)
{
    const auto trimmed = TrimTrailingSlashes(_path);
    if( trimmed == "/" )
        return {};
    return trimmed.substr(trimmed.rfind('/') + 1);
}

std::string JoinPath(std::string_view _directory, std::string_view _name)
{
    auto result = TrimTrailingSlashes(_directory);
    if( result != "/" )
        result.push_back('/');
    result.append(_name);
    return result;
}

std::string FoldASCII(std::string _value)
{
    std::ranges::transform(_value, _value.begin(), [](unsigned char _character) {
        return static_cast<char>(std::tolower(_character));
    });
    return _value;
}

bool ContainsNonASCII(std::string_view _value) noexcept
{
    return std::ranges::any_of(_value, [](unsigned char _character) { return _character >= 0x80; });
}

bool SamePath(const OperationPlanningPath &_lhs,
              const OperationPlanningPath &_rhs,
              bool _case_sensitive)
{
    if( _lhs.provider_id != _rhs.provider_id )
        return false;
    const auto lhs = NormalizeAbsolutePath(_lhs.absolute_path);
    const auto rhs = NormalizeAbsolutePath(_rhs.absolute_path);
    return _case_sensitive ? lhs == rhs : FoldASCII(lhs) == FoldASCII(rhs);
}

bool IsDescendant(const OperationPlanningPath &_ancestor,
                  const OperationPlanningPath &_candidate,
                  bool _case_sensitive)
{
    if( _ancestor.provider_id != _candidate.provider_id )
        return false;
    auto ancestor = NormalizeAbsolutePath(_ancestor.absolute_path);
    auto candidate = NormalizeAbsolutePath(_candidate.absolute_path);
    if( !_case_sensitive ) {
        ancestor = FoldASCII(std::move(ancestor));
        candidate = FoldASCII(std::move(candidate));
    }
    if( ancestor == "/" )
        return candidate != "/";
    return candidate.size() > ancestor.size() && candidate.starts_with(ancestor) &&
           candidate[ancestor.size()] == '/';
}

OperationPlanningBlockerCode ProbeBlocker(OperationPlanningProbeError _error)
{
    switch( _error ) {
        case OperationPlanningProbeError::Unsupported:
        case OperationPlanningProbeError::UnsupportedItem:
            return OperationPlanningBlockerCode::ProviderCapabilityUnsupported;
        case OperationPlanningProbeError::InvalidName:
            return OperationPlanningBlockerCode::InvalidDestinationName;
        case OperationPlanningProbeError::Unavailable:
            return OperationPlanningBlockerCode::ProviderUnavailable;
        case OperationPlanningProbeError::Cancelled:
            return OperationPlanningBlockerCode::ProbeCancelled;
        case OperationPlanningProbeError::PermissionDenied:
            return OperationPlanningBlockerCode::PermissionDenied;
        case OperationPlanningProbeError::Failed:
            return OperationPlanningBlockerCode::ProbeFailed;
    }
    return OperationPlanningBlockerCode::ProbeFailed;
}

bool IsValid(OperationPlanningItemKind _kind) noexcept
{
    switch( _kind ) {
        case OperationPlanningItemKind::Missing:
        case OperationPlanningItemKind::File:
        case OperationPlanningItemKind::Directory:
        case OperationPlanningItemKind::Symlink:
        case OperationPlanningItemKind::Other:
            return true;
    }
    return false;
}

bool IsValid(OperationPlanningPathIdentitySemantics _semantics) noexcept
{
    switch( _semantics ) {
        case OperationPlanningPathIdentitySemantics::ExactBytes:
        case OperationPlanningPathIdentitySemantics::ASCIICaseSensitive:
        case OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive:
        case OperationPlanningPathIdentitySemantics::Unavailable:
            return true;
    }
    return false;
}

OperationPlanningPathIdentitySemantics
Combine(OperationPlanningPathIdentitySemantics _lhs,
        OperationPlanningPathIdentitySemantics _rhs) noexcept
{
    if( _lhs == OperationPlanningPathIdentitySemantics::Unavailable ||
        _rhs == OperationPlanningPathIdentitySemantics::Unavailable )
        return OperationPlanningPathIdentitySemantics::Unavailable;
    if( _lhs == OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive ||
        _rhs == OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive )
        return OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
    if( _lhs == OperationPlanningPathIdentitySemantics::ASCIICaseSensitive ||
        _rhs == OperationPlanningPathIdentitySemantics::ASCIICaseSensitive )
        return OperationPlanningPathIdentitySemantics::ASCIICaseSensitive;
    return OperationPlanningPathIdentitySemantics::ExactBytes;
}

bool SupportsComparison(OperationPlanningPathIdentitySemantics _semantics,
                        std::string_view _lhs,
                        std::string_view _rhs) noexcept
{
    if( _semantics == OperationPlanningPathIdentitySemantics::Unavailable )
        return false;
    if( _semantics == OperationPlanningPathIdentitySemantics::ExactBytes )
        return true;
    return !ContainsNonASCII(_lhs) && !ContainsNonASCII(_rhs);
}

} // namespace

class OperationPlanningRun
{
public:
    OperationPlanningRun(OperationPlan _plan, OperationPlanningProbes &_probes)
        : m_Plan(std::move(_plan)), m_Probes(_probes)
    {
    }

    OperationPreflightResult Run()
    {
        if( m_Plan.Type() == OperationPlanType::Move )
            return RunMove();
        if( m_Plan.Type() != OperationPlanType::Copy ) {
            AddBlocker(OperationPlanningBlockerCode::UnsupportedPlanType, std::nullopt);
            return Block();
        }

        const auto policy_decision = m_Plan.ConflictPolicy()->Decision();
        const auto policy_scope = m_Plan.ConflictPolicy()->Scope();
        if( policy_decision == OperationPlanConflictDecision::KeepBoth ||
            policy_decision == OperationPlanConflictDecision::RenameNew ||
            policy_decision == OperationPlanConflictDecision::RenameExisting ||
            policy_decision == OperationPlanConflictDecision::MergeFolders ||
            (policy_decision != OperationPlanConflictDecision::Ask &&
             (policy_scope == OperationPlanConflictScope::SameExtension ||
              policy_scope == OperationPlanConflictScope::SameFolder ||
              (policy_scope == OperationPlanConflictScope::ThisItem && m_Plan.Sources().size() != 1))) ) {
            AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported, std::nullopt);
            return Block();
        }

        const auto &destination = *m_Plan.Destination();
        const auto destination_directory = OperationPlanningPath{
            .provider_id = std::string{destination.ProviderId().Value()},
            .absolute_path = destination.Kind() == OperationPlanDestinationKind::Directory
                                 ? TrimTrailingSlashes(destination.AbsolutePath())
                                 : ParentPath(destination.AbsolutePath()),
        };

        const auto *destination_provider = Provider(destination_directory);
        const bool destination_capable = destination_provider && destination_provider->can_copy_to;
        if( destination_provider && !destination_provider->can_copy_to )
            AddBlocker(OperationPlanningBlockerCode::DestinationNotWritable, destination_directory);

        bool destination_ready = false;
        if( destination_capable ) {
            const auto *destination_item = Item(destination_directory);
            if( destination_item ) {
                if( destination_item->kind == OperationPlanningItemKind::Missing )
                    AddBlocker(OperationPlanningBlockerCode::DestinationMissing, destination_directory);
                else if( destination_item->kind != OperationPlanningItemKind::Directory )
                    AddBlocker(OperationPlanningBlockerCode::DestinationNotDirectory, destination_directory);
                else if( const auto *access = Access(destination_directory, OperationPlanningRequiredAccess::Write);
                         access && access->state == OperationPlanningAccessState::Granted )
                    destination_ready = true;
            }
        }

        for( const auto &source : m_Plan.Sources() ) {
            if( m_Cancelled )
                break;
            PlanSource(source, destination, destination_provider, destination_ready);
        }

        CalculateTotals();
        if( !m_Cancelled && !m_Report.items.empty() )
            CheckSpace(destination_directory);

        AddWarning(OperationPlanningWarningCode::RuntimeRevalidationRequired, std::nullopt);
        if( m_Report.items.empty() && m_Blockers.empty() )
            AddBlocker(OperationPlanningBlockerCode::NothingToDo, std::nullopt);

        if( m_Blockers.empty() )
            return AcceptedOperationPlan{std::move(m_Plan), std::move(m_Report)};
        return Block();
    }

private:
    using ProviderResult = OperationPlanningProbeResult<OperationPlanningProviderEvidence>;
    using ItemResult = OperationPlanningProbeResult<OperationPlanningItemEvidence>;
    using NameResult = OperationPlanningProbeResult<OperationPlanningNameEvidence>;
    using AccessResult = OperationPlanningProbeResult<OperationPlanningAccessEvidence>;
    using EstimateResult = OperationPlanningProbeResult<OperationPlanningEstimateEvidence>;
    using SpaceResult = OperationPlanningProbeResult<OperationPlanningSpaceEvidence>;

    /**
     * The first Move preflight is intentionally an intent-only, one-file rename shape. It binds the
     * exact source and destination paths plus their parent-namespace capability/access evidence, but
     * it creates no execution authority and does not promise cross-provider or replacement behavior.
     */
    OperationPreflightResult RunMove()
    {
        const auto &sources = m_Plan.Sources();
        const auto &destination = *m_Plan.Destination();
        const auto &conflict_policy = *m_Plan.ConflictPolicy();
        if( sources.size() != 1 || destination.Kind() != OperationPlanDestinationKind::ExactItem ) {
            AddBlocker(OperationPlanningBlockerCode::UnsupportedPlanType, std::nullopt);
            return Block();
        }
        if( conflict_policy.Decision() != OperationPlanConflictDecision::Ask ||
            conflict_policy.Scope() != OperationPlanConflictScope::ThisItem ) {
            AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported, std::nullopt);
            return Block();
        }

        const OperationPlanningPath source_path{
            .provider_id = std::string{sources.front().ProviderId().Value()},
            .absolute_path = std::string{sources.front().AbsolutePath()},
        };
        const OperationPlanningPath destination_path{
            .provider_id = std::string{destination.ProviderId().Value()},
            .absolute_path = std::string{destination.AbsolutePath()},
        };
        if( source_path.provider_id != destination_path.provider_id ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, destination_path);
            return FinishMove();
        }

        const auto source_name = BaseName(source_path.absolute_path);
        if( source_name.empty() ) {
            AddBlocker(OperationPlanningBlockerCode::InvalidSourceName, source_path);
            return FinishMove();
        }
        const OperationPlanningPath source_parent{
            .provider_id = source_path.provider_id,
            .absolute_path = ParentPath(source_path.absolute_path),
        };
        const OperationPlanningPath destination_parent{
            .provider_id = destination_path.provider_id,
            .absolute_path = ParentPath(destination_path.absolute_path),
        };

        const auto *source_provider = Provider(source_parent);
        const auto *destination_provider = Provider(destination_parent);
        if( !source_provider || !destination_provider )
            return FinishMove();
        if( !source_provider->can_rename ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, source_parent);
            return FinishMove();
        }
        if( !destination_provider->can_rename ) {
            AddBlocker(OperationPlanningBlockerCode::DestinationNotWritable, destination_parent);
            return FinishMove();
        }

        const auto *source_parent_item = Item(source_parent);
        if( !source_parent_item )
            return FinishMove();
        if( source_parent_item->kind != OperationPlanningItemKind::Directory ) {
            AddBlocker(OperationPlanningBlockerCode::SourceMissing, source_parent);
            return FinishMove();
        }
        const auto *destination_parent_item = Item(destination_parent);
        if( !destination_parent_item )
            return FinishMove();
        if( destination_parent_item->kind == OperationPlanningItemKind::Missing ) {
            AddBlocker(OperationPlanningBlockerCode::DestinationMissing, destination_parent);
            return FinishMove();
        }
        if( destination_parent_item->kind != OperationPlanningItemKind::Directory ) {
            AddBlocker(OperationPlanningBlockerCode::DestinationNotDirectory, destination_parent);
            return FinishMove();
        }

        const auto *source_item = Item(source_path);
        if( !source_item )
            return FinishMove();
        if( source_item->kind == OperationPlanningItemKind::Missing ) {
            AddBlocker(OperationPlanningBlockerCode::SourceMissing, source_path);
            return FinishMove();
        }
        if( source_item->kind != OperationPlanningItemKind::File ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, source_path);
            return FinishMove();
        }

        const auto *destination_name = Name(destination_path);
        if( !destination_name )
            return FinishMove();
        if( !destination_name->valid ) {
            AddBlocker(OperationPlanningBlockerCode::InvalidDestinationName, destination_path);
            return FinishMove();
        }

        const auto *source_access = Access(source_parent, OperationPlanningRequiredAccess::Rename);
        if( !source_access || source_access->state != OperationPlanningAccessState::Granted )
            return FinishMove();
        const auto *destination_access = Access(destination_parent, OperationPlanningRequiredAccess::Rename);
        if( !destination_access || destination_access->state != OperationPlanningAccessState::Granted )
            return FinishMove();
        if( m_Cancelled )
            return FinishMove();

        const auto comparison_identity = Combine(source_provider->path_identity, destination_provider->path_identity);
        if( !SupportsComparison(comparison_identity, source_path.absolute_path, destination_path.absolute_path) ) {
            AddBlocker(OperationPlanningBlockerCode::PathIdentityUnavailable, destination_path);
            return FinishMove();
        }
        const bool case_sensitive = comparison_identity != OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
        if( SamePath(source_path, destination_path, case_sensitive) ) {
            AddBlocker(OperationPlanningBlockerCode::SamePath, destination_path);
            return FinishMove();
        }

        const auto *destination_item = Item(destination_path);
        if( !destination_item )
            return FinishMove();
        if( destination_item->kind != OperationPlanningItemKind::Missing ) {
            m_Report.conflicts.emplace_back(
                OperationPlanningConflict{source_path, destination_path, conflict_policy.Decision()});
            AddBlocker(OperationPlanningBlockerCode::ConflictDecisionRequired, destination_path);
            return FinishMove();
        }

        std::optional<OperationPlanningEstimateEvidence> estimate;
        if( source_item->byte_size )
            estimate = OperationPlanningEstimateEvidence{.files = 1, .bytes = *source_item->byte_size};
        m_Report.items.emplace_back(OperationPlannedCopyItem{
            .source = source_path,
            .destination = destination_path,
            .source_kind = source_item->kind,
            .estimate = estimate,
        });
        return FinishMove();
    }

    OperationPreflightResult FinishMove()
    {
        CalculateTotals();
        AddWarning(OperationPlanningWarningCode::RuntimeRevalidationRequired, std::nullopt);
        if( !m_Cancelled && m_Report.items.empty() && m_Blockers.empty() )
            AddBlocker(OperationPlanningBlockerCode::NothingToDo, std::nullopt);
        if( m_Blockers.empty() )
            return AcceptedOperationPlan{std::move(m_Plan), std::move(m_Report)};
        return Block();
    }

    void PlanSource(const OperationPlanSource &_source,
                    const OperationPlanDestination &_destination,
                    const OperationPlanningProviderEvidence *_destination_provider,
                    bool _destination_ready)
    {
        const OperationPlanningPath source_path{
            .provider_id = std::string{_source.ProviderId().Value()},
            .absolute_path = std::string{_source.AbsolutePath()},
        };
        const auto name = BaseName(source_path.absolute_path);
        if( name.empty() ) {
            AddBlocker(OperationPlanningBlockerCode::InvalidSourceName, source_path);
            return;
        }

        const OperationPlanningPath effective_destination{
            .provider_id = std::string{_destination.ProviderId().Value()},
            .absolute_path = _destination.Kind() == OperationPlanDestinationKind::Directory
                                 ? JoinPath(_destination.AbsolutePath(), name)
                                 : std::string{_destination.AbsolutePath()},
        };

        const auto *source_provider = Provider(source_path);
        if( !source_provider )
            return;
        if( !source_provider->can_copy_from ) {
            AddBlocker(OperationPlanningBlockerCode::SourceUnreadable, source_path);
            return;
        }

        const auto *source_item = Item(source_path);
        if( !source_item )
            return;
        if( source_item->kind == OperationPlanningItemKind::Missing ) {
            AddBlocker(OperationPlanningBlockerCode::SourceMissing, source_path);
            return;
        }
        if( source_item->kind == OperationPlanningItemKind::Other ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, source_path);
            return;
        }
        if( source_item->kind == OperationPlanningItemKind::Symlink &&
            (!_destination_provider || !_destination_provider->can_copy_symlink_to) ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, effective_destination);
            return;
        }
        const bool destination_name_is_derived_from_same_provider =
            _destination.Kind() == OperationPlanDestinationKind::Directory &&
            source_path.provider_id == effective_destination.provider_id;
        if( !destination_name_is_derived_from_same_provider ) {
            const auto *destination_name = Name(effective_destination);
            if( !destination_name )
                return;
            if( !destination_name->valid ) {
                AddBlocker(OperationPlanningBlockerCode::InvalidDestinationName, effective_destination);
                return;
            }
        }

        const auto *source_access = Access(source_path, OperationPlanningRequiredAccess::Read);
        if( !source_access || source_access->state != OperationPlanningAccessState::Granted )
            return;
        if( m_Cancelled )
            return;

        const auto destination_identity = _destination_provider
                                              ? _destination_provider->path_identity
                                              : OperationPlanningPathIdentitySemantics::Unavailable;
        if( source_path.provider_id == effective_destination.provider_id ) {
            const auto comparison_identity = Combine(source_provider->path_identity, destination_identity);
            if( !SupportsComparison(comparison_identity,
                                    source_path.absolute_path,
                                    effective_destination.absolute_path) ) {
                AddBlocker(OperationPlanningBlockerCode::PathIdentityUnavailable, effective_destination);
                return;
            }
            const bool case_sensitive =
                comparison_identity != OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
            if( SamePath(source_path, effective_destination, case_sensitive) ) {
                AddBlocker(OperationPlanningBlockerCode::SamePath, effective_destination);
                return;
            }
            if( source_item->kind == OperationPlanningItemKind::Directory &&
                IsDescendant(source_path, effective_destination, case_sensitive) ) {
                AddBlocker(OperationPlanningBlockerCode::RecursiveDestination, effective_destination);
                return;
            }
        }

        if( !_destination_ready )
            return;

        const auto *destination_item = Item(effective_destination);
        if( !destination_item )
            return;
        if( destination_item->kind == OperationPlanningItemKind::Other ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, effective_destination);
            return;
        }

        if( destination_item->kind != OperationPlanningItemKind::Missing ) {
            const auto decision = m_Plan.ConflictPolicy()->Decision();
            m_Report.conflicts.emplace_back(OperationPlanningConflict{source_path, effective_destination, decision});
            switch( decision ) {
                case OperationPlanConflictDecision::Ask:
                    AddBlocker(OperationPlanningBlockerCode::ConflictDecisionRequired, effective_destination);
                    return;
                case OperationPlanConflictDecision::Replace: {
                    if( source_item->kind != destination_item->kind ) {
                        AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported,
                                   effective_destination);
                        return;
                }
                    if( source_item->kind == OperationPlanningItemKind::Directory ) {
                        AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported,
                                   effective_destination);
                        return;
                    }
                    const auto *replacement_provider = Provider(effective_destination);
                    if( !replacement_provider )
                        return;
                    const bool replacing_directory =
                        destination_item->kind == OperationPlanningItemKind::Directory;
                    const bool replacement_supported = replacing_directory
                                                           ? replacement_provider->can_replace_directory
                                                           : replacement_provider->can_replace_file;
                    if( !replacement_supported ) {
                        AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported,
                                   effective_destination);
                        return;
                    }
                    const auto required_access = replacing_directory
                                                     ? OperationPlanningRequiredAccess::ReplaceDirectory
                                                     : OperationPlanningRequiredAccess::ReplaceFile;
                    const auto *replacement_access = Access(effective_destination, required_access);
                    if( !replacement_access ||
                        replacement_access->state != OperationPlanningAccessState::Granted )
                        return;
                    m_Report.destructive_effects.emplace_back(
                        OperationPlanningDestructiveEffect{effective_destination});
                    AddWarning(OperationPlanningWarningCode::DestructiveReplacement, effective_destination);
                    m_Report.requires_confirmation = true;
                    break;
                    }
                case OperationPlanConflictDecision::Skip:
                    return;
                case OperationPlanConflictDecision::MergeFolders:
                    AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported, effective_destination);
                    return;
                case OperationPlanConflictDecision::KeepBoth:
                case OperationPlanConflictDecision::RenameNew:
                case OperationPlanConflictDecision::RenameExisting:
                    AddBlocker(OperationPlanningBlockerCode::ConflictPolicyUnsupported, effective_destination);
                    return;
            }
        }

        if( m_Plan.Sources().size() > 1 ) {
            if( !SupportsComparison(destination_identity,
                                    effective_destination.absolute_path,
                                    effective_destination.absolute_path) ) {
                AddBlocker(OperationPlanningBlockerCode::PathIdentityUnavailable, effective_destination);
                return;
            }
            auto identity_key = effective_destination.provider_id;
            identity_key.push_back('\0');
            auto comparable_path = NormalizeAbsolutePath(effective_destination.absolute_path);
            if( destination_identity == OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive )
                comparable_path = FoldASCII(std::move(comparable_path));
            identity_key += comparable_path;
            if( !m_EffectiveDestinations.emplace(std::move(identity_key)).second ) {
                AddBlocker(OperationPlanningBlockerCode::DuplicateDestination, effective_destination);
                return;
            }
        }

        const size_t blocker_count_before_estimate = m_Blockers.size();
        std::optional<OperationPlanningEstimateEvidence> estimate;
        if( (source_item->kind == OperationPlanningItemKind::File ||
             source_item->kind == OperationPlanningItemKind::Symlink) &&
            source_item->byte_size ) {
            estimate = OperationPlanningEstimateEvidence{.files = 1, .bytes = *source_item->byte_size};
        }
        else if( const auto *value = Estimate(source_path, effective_destination) ) {
            estimate = *value;
        }
        if( estimate && estimate->contains_symlinks &&
            (!_destination_provider || !_destination_provider->can_copy_symlink_to) ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, effective_destination);
            return;
        }
        if( source_item->kind == OperationPlanningItemKind::Directory && !estimate &&
            (!_destination_provider || !_destination_provider->can_copy_symlink_to) ) {
            AddBlocker(OperationPlanningBlockerCode::ProviderCapabilityUnsupported, effective_destination);
            return;
        }
        if( source_item->kind == OperationPlanningItemKind::Directory && !estimate &&
            source_path.provider_id != effective_destination.provider_id &&
            m_Blockers.size() == blocker_count_before_estimate ) {
            AddBlocker(OperationPlanningBlockerCode::DestinationNameEvidenceUnavailable,
                       effective_destination);
            return;
        }

        m_Report.items.emplace_back(OperationPlannedCopyItem{
            .source = source_path,
            .destination = effective_destination,
            .source_kind = source_item->kind,
            .estimate = estimate,
        });
    }

    const OperationPlanningProviderEvidence *Provider(const OperationPlanningPath &_path)
    {
        const auto key = Key(_path);
        if( const auto found = m_ProviderCache.find(key); found != m_ProviderCache.end() )
            return found->second ? &*found->second : nullptr;

        ProviderResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeProvider(_path);
        }
        catch( ... ) {
        }
        const auto [entry, inserted] = m_ProviderCache.emplace(key, std::move(result));
        (void)inserted;
        if( !entry->second ) {
            AddProbeBlocker(entry->second.error(), _path);
            return nullptr;
        }
        if( !IsValid(entry->second->path_identity) ) {
            AddBlocker(OperationPlanningBlockerCode::ProbeFailed, _path);
            return nullptr;
        }
        m_Report.provider_evidence.emplace_back(OperationPlanningProviderSnapshot{_path, *entry->second});
        return &*entry->second;
    }

    const OperationPlanningItemEvidence *Item(const OperationPlanningPath &_path)
    {
        const auto key = ItemKey(_path);
        if( const auto found = m_ItemCache.find(key); found != m_ItemCache.end() )
            return found->second ? &*found->second : nullptr;

        ItemResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeItem(_path);
        }
        catch( ... ) {
        }
        const auto [entry, inserted] = m_ItemCache.emplace(key, std::move(result));
        (void)inserted;
        if( !entry->second ) {
            AddProbeBlocker(entry->second.error(), _path);
            return nullptr;
        }
        if( !IsValid(entry->second->kind) ) {
            AddBlocker(OperationPlanningBlockerCode::ProbeFailed, _path);
            return nullptr;
        }
        m_Report.item_evidence.emplace_back(OperationPlanningItemSnapshot{
            .path = OperationPlanningPath{
                .provider_id = _path.provider_id,
                .absolute_path = NormalizeAbsolutePath(_path.absolute_path),
            },
            .evidence = *entry->second,
        });
        return &*entry->second;
    }

    const OperationPlanningNameEvidence *Name(const OperationPlanningPath &_path)
    {
        const auto key = Key(_path);
        if( const auto found = m_NameCache.find(key); found != m_NameCache.end() )
            return found->second ? &*found->second : nullptr;

        NameResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeDestinationName(_path);
        }
        catch( ... ) {
        }
        const auto [entry, inserted] = m_NameCache.emplace(key, std::move(result));
        (void)inserted;
        if( !entry->second ) {
            AddProbeBlocker(entry->second.error(), _path);
            return nullptr;
        }
        m_Report.name_evidence.emplace_back(OperationPlanningNameSnapshot{_path, *entry->second});
        return &*entry->second;
    }

    const OperationPlanningAccessEvidence *Access(const OperationPlanningPath &_path,
                                                  OperationPlanningRequiredAccess _required)
    {
        const auto key = AccessKey(_path, _required);
        if( const auto found = m_AccessCache.find(key); found != m_AccessCache.end() )
            return found->second ? &*found->second : nullptr;

        AccessResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeAccess(_path, _required);
        }
        catch( ... ) {
        }
        const auto [entry, inserted] = m_AccessCache.emplace(key, std::move(result));
        (void)inserted;
        if( !entry->second ) {
            AddProbeBlocker(entry->second.error(), _path);
            return nullptr;
        }
        m_Report.access_evidence.emplace_back(OperationPlanningAccessSnapshot{_path, _required, *entry->second});
        switch( entry->second->state ) {
            case OperationPlanningAccessState::Granted:
                break;
            case OperationPlanningAccessState::PermissionRequired:
                AddBlocker(OperationPlanningBlockerCode::PermissionRequired, _path);
                break;
            case OperationPlanningAccessState::Denied:
                AddBlocker(OperationPlanningBlockerCode::PermissionDenied, _path);
                break;
            default:
                AddBlocker(OperationPlanningBlockerCode::ProbeFailed, _path);
                break;
        }
        return &*entry->second;
    }

    const OperationPlanningEstimateEvidence *Estimate(const OperationPlanningPath &_path,
                                                       const OperationPlanningPath &_destination)
    {
        auto key = Key(_path);
        key.push_back('\0');
        key += Key(_destination);
        if( const auto found = m_EstimateCache.find(key); found != m_EstimateCache.end() )
            return found->second ? &*found->second : nullptr;

        EstimateResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeEstimate(_path, _destination);
        }
        catch( ... ) {
        }
        const auto [entry, inserted] = m_EstimateCache.emplace(key, std::move(result));
        (void)inserted;
        if( !entry->second ) {
            if( entry->second.error() == OperationPlanningProbeError::Unsupported )
                AddWarning(OperationPlanningWarningCode::EstimateUnavailable, _path);
            else
                AddProbeBlocker(entry->second.error(), _path);
            return nullptr;
        }
        return &*entry->second;
    }

    void CheckSpace(const OperationPlanningPath &_path)
    {
        SpaceResult result = std::unexpected(OperationPlanningProbeError::Failed);
        try {
            result = m_Probes.ProbeSpace(_path);
        }
        catch( ... ) {
        }
        if( !result ) {
            if( result.error() == OperationPlanningProbeError::Unsupported )
                AddWarning(OperationPlanningWarningCode::SpaceUnknown, _path);
            else
                AddProbeBlocker(result.error(), _path);
            return;
        }
        m_Report.destination_space = *result;
        if( !result->available_bytes ) {
            AddWarning(OperationPlanningWarningCode::SpaceUnknown, _path);
            return;
        }
        if( m_Report.estimated_bytes && *result->available_bytes < *m_Report.estimated_bytes )
            AddBlocker(OperationPlanningBlockerCode::InsufficientSpace, _path);
    }

    void CalculateTotals()
    {
        uint64_t files = 0;
        uint64_t bytes = 0;
        bool complete = true;
        for( const auto &item : m_Report.items ) {
            if( !item.estimate ) {
                complete = false;
                continue;
            }
            if( item.estimate->files > std::numeric_limits<uint64_t>::max() - files ||
                item.estimate->bytes > std::numeric_limits<uint64_t>::max() - bytes ) {
                AddBlocker(OperationPlanningBlockerCode::EstimateOverflow, item.source);
                complete = false;
                continue;
            }
            files += item.estimate->files;
            bytes += item.estimate->bytes;
        }
        if( complete ) {
            m_Report.estimated_files = files;
            m_Report.estimated_bytes = bytes;
        }
    }

    void AddProbeBlocker(OperationPlanningProbeError _error, const OperationPlanningPath &_path)
    {
        const auto code = ProbeBlocker(_error);
        AddBlocker(code, _path);
        if( code == OperationPlanningBlockerCode::ProbeCancelled )
            m_Cancelled = true;
    }

    void AddBlocker(OperationPlanningBlockerCode _code, std::optional<OperationPlanningPath> _path)
    {
        const OperationPlanningBlocker blocker{_code, std::move(_path)};
        if( std::ranges::find(m_Blockers, blocker) == m_Blockers.end() )
            m_Blockers.emplace_back(blocker);
    }

    void AddWarning(OperationPlanningWarningCode _code, std::optional<OperationPlanningPath> _path)
    {
        const OperationPlanningWarning warning{_code, std::move(_path)};
        if( std::ranges::find(m_Report.warnings, warning) == m_Report.warnings.end() )
            m_Report.warnings.emplace_back(warning);
    }

    OperationPreflightResult Block()
    {
        return BlockedOperationPlan{std::move(m_Plan), std::move(m_Report), std::move(m_Blockers)};
    }

    OperationPlan m_Plan;
    OperationPlanningProbes &m_Probes;
    OperationPreflightReport m_Report;
    std::vector<OperationPlanningBlocker> m_Blockers;
    std::map<std::string, ProviderResult> m_ProviderCache;
    std::map<std::string, ItemResult> m_ItemCache;
    std::map<std::string, NameResult> m_NameCache;
    std::map<std::string, AccessResult> m_AccessCache;
    std::map<std::string, EstimateResult> m_EstimateCache;
    std::set<std::string> m_EffectiveDestinations;
    bool m_Cancelled = false;
};

AcceptedOperationPlan::AcceptedOperationPlan(OperationPlan _plan, OperationPreflightReport _report)
    : m_Plan(std::move(_plan)), m_Report(std::move(_report))
{
}

BlockedOperationPlan::BlockedOperationPlan(OperationPlan _plan,
                                           OperationPreflightReport _report,
                                           std::vector<OperationPlanningBlocker> _blockers)
    : m_Plan(std::move(_plan)), m_Report(std::move(_report)), m_Blockers(std::move(_blockers))
{
    assert(!m_Blockers.empty());
}

OperationPreflightResult OperationPlanner::Preflight(OperationPlan _plan, OperationPlanningProbes &_probes)
{
    OperationPlanningRun run{std::move(_plan), _probes};
    return run.Run();
}

} // namespace nc::ops
