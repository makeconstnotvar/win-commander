// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "InlineRename.h"

#include <Operations/Copying.h>
#include <VFS/ProviderCapabilities.h>
#include <sys/stat.h>
#include <utility>

namespace nc::panel::actions {
namespace {

[[nodiscard]] std::optional<std::string> FilenameKey(const std::string_view _filename,
                                                     const bool _case_sensitive) noexcept
{
    @autoreleasepool {
        NSString *string = [[NSString alloc] initWithBytes:_filename.data()
                                                   length:_filename.size()
                                                 encoding:NSUTF8StringEncoding];
        if( string == nil )
            return std::nullopt;
        string = string.precomposedStringWithCanonicalMapping;
        if( !_case_sensitive )
            string = string.lowercaseString;
        const char *const utf8 = string.UTF8String;
        if( utf8 == nullptr )
            return std::nullopt;
        return std::string{utf8};
    }
}

[[nodiscard]] std::string ChildPath(std::string _directory, const std::string_view _name)
{
    if( _directory.empty() || _directory.back() != '/' )
        _directory.push_back('/');
    _directory.append(_name);
    return _directory;
}

[[nodiscard]] bool SourceStatMatches(const VFSListingItem &_source, const VFSStat &_stat) noexcept
{
    if( !_stat.meaning.mode )
        return false;

    const mode_t expected_type = _source.IsSymlink() ? S_IFLNK : (_source.UnixMode() & S_IFMT);
    if( (_stat.mode & S_IFMT) != expected_type )
        return false;

    if( _source.HasInode() && (!_stat.meaning.inode || _stat.inode != _source.Inode()) )
        return false;
    return true;
}

[[nodiscard]] bool StatsIdentifySameNativeEntry(const VFSStat &_lhs, const VFSStat &_rhs) noexcept
{
    return _lhs.meaning.dev && _lhs.meaning.inode && _rhs.meaning.dev && _rhs.meaning.inode &&
           _lhs.dev == _rhs.dev && _lhs.inode == _rhs.inode;
}

[[nodiscard]] bool MissingAtPath(const VFSHostPtr &_host, const std::expected<VFSStat, nc::Error> &_stat) noexcept
{
    return !_stat && _host->ClassifyError(_stat.error()) == vfs::HostErrorKind::Missing;
}

[[nodiscard]] InlineRenameStatus CheckProvider(const VFSHostPtr &_host,
                                               const std::string &_directory,
                                               std::optional<bool> &_case_sensitive) noexcept
{
    try {
        if( !_host->IsWritableAtPath(_directory) )
            return InlineRenameStatus::DestinationReadOnly;
        if( !vfs::ProviderCapabilitiesResolver::Resolve(*_host, _directory).can_rename )
            return InlineRenameStatus::ProviderUnsupported;
        _case_sensitive = _host->CaseSensitivityAtPath(_directory);
        if( !_case_sensitive )
            return InlineRenameStatus::CaseSensitivityUnavailable;
        return InlineRenameStatus::Ready;
    } catch( ... ) {
        return InlineRenameStatus::ProviderUnsupported;
    }
}

} // namespace

InlineRenamePlan::InlineRenamePlan(VFSListingItem _source,
                                   VFSListingPtr _listing,
                                   const unsigned long _generation,
                                   VFSHostPtr _host,
                                   std::string _directory,
                                   std::string _destination_name,
                                   std::string _destination_path,
                                   const bool _case_sensitive,
                                   const bool _case_only_rename) noexcept
    : m_Source{std::move(_source)}, m_Listing{std::move(_listing)}, m_Generation{_generation},
      m_Host{std::move(_host)}, m_Directory{std::move(_directory)},
      m_DestinationName{std::move(_destination_name)}, m_DestinationPath{std::move(_destination_path)},
      m_CaseSensitive{_case_sensitive}, m_CaseOnlyRename{_case_only_rename}
{
}

InlineRenamePlanningResult PlanInlineRename(const InlineRenameLiveContext &_live,
                                            const VFSListingItem &_source,
                                            const std::string_view _destination_name) noexcept
{
    try {
        if( !_live.pane_available )
            return {.status = InlineRenameStatus::PaneUnavailable};
        if( !_live.window_available )
            return {.status = InlineRenameStatus::WindowUnavailable};
        if( _live.loading )
            return {.status = InlineRenameStatus::Loading};
        if( !_live.listing_loaded || !_live.uniform || !_live.listing || !_live.listing->IsUniform() ||
            !_live.host || _live.directory.empty() ) {
            return {.status = InlineRenameStatus::ListingUnavailable};
        }
        if( !_source || _source.IsDotDot() || _source.Listing().get() != _live.listing.get() ||
            _source.Index() >= _live.listing->Count() || _live.listing->Item(_source.Index()) != _source ||
            _source.Host() != _live.host || _source.Directory() != _live.directory ) {
            return {.status = InlineRenameStatus::StaleSource};
        }
        if( _destination_name == _source.Filename() )
            return {.status = InlineRenameStatus::Unchanged};
        if( _destination_name.empty() || _destination_name.find('\0') != std::string_view::npos ||
            _destination_name == "." || _destination_name == ".." ||
            !_live.host->ValidateFilename(_destination_name) ) {
            return {.status = InlineRenameStatus::InvalidName};
        }

        std::optional<bool> case_sensitive;
        if( const InlineRenameStatus provider = CheckProvider(_live.host, _live.directory, case_sensitive);
            provider != InlineRenameStatus::Ready ) {
            return {.status = provider};
        }

        const auto source_key = FilenameKey(_source.Filename(), *case_sensitive);
        const auto destination_key = FilenameKey(_destination_name, *case_sensitive);
        if( !source_key || !destination_key )
            return {.status = InlineRenameStatus::InvalidName};

        const bool same_source_key = *source_key == *destination_key;
        const bool case_only_rename = !case_sensitive.value() && same_source_key;
        if( case_only_rename && !_live.host->IsNativeFS() )
            return {.status = InlineRenameStatus::UnsafeCaseOnlyRename};
        if( *case_sensitive && same_source_key )
            return {.status = InlineRenameStatus::DestinationExists};

        for( const VFSListingItem &item : *_live.listing ) {
            if( item == _source || item.IsDotDot() )
                continue;
            const auto existing_key = FilenameKey(item.Filename(), *case_sensitive);
            if( !existing_key )
                return {.status = InlineRenameStatus::ListingUnavailable};
            if( *existing_key == *destination_key )
                return {.status = InlineRenameStatus::DestinationExists};
        }

        std::string destination_name{_destination_name};
        std::string destination_path = ChildPath(_live.directory, destination_name);
        InlineRenamePlan plan{_source,
                              _live.listing,
                              _live.generation,
                              _live.host,
                              _live.directory,
                              std::move(destination_name),
                              std::move(destination_path),
                              *case_sensitive,
                              case_only_rename};
        return {.status = InlineRenameStatus::Ready, .plan = std::move(plan)};
    } catch( ... ) {
        return {.status = InlineRenameStatus::ListingUnavailable};
    }
}

bool RevalidateInlineRenameRuntime(const InlineRenamePlan &_plan) noexcept
{
    try {
        const VFSHostPtr &host = _plan.Host();
        if( !host || !_plan.Source() || _plan.Source().IsDotDot() || !_plan.Listing() ||
            _plan.Source().Listing().get() != _plan.Listing().get() ||
            _plan.Source().Index() >= _plan.Listing()->Count() ||
            _plan.Listing()->Item(_plan.Source().Index()) != _plan.Source() || _plan.Source().Host() != host ||
            _plan.Source().Directory() != _plan.Directory() ||
            (!_plan.DestinationName().empty() && !host->ValidateFilename(_plan.DestinationName())) ) {
            return false;
        }
        if( _plan.DestinationName().empty() || _plan.DestinationName().find('\0') != std::string::npos ||
            _plan.DestinationName() == "." ||
            _plan.DestinationName() == ".." || _plan.DestinationPath() !=
                                                    ChildPath(_plan.Directory(), _plan.DestinationName()) ) {
            return false;
        }

        std::optional<bool> case_sensitive;
        if( CheckProvider(host, _plan.Directory(), case_sensitive) != InlineRenameStatus::Ready ||
            *case_sensitive != _plan.CaseSensitive() ) {
            return false;
        }

        const auto source_stat = host->Stat(_plan.Source().Path(), VFSFlags::F_NoFollow);
        if( !source_stat || !SourceStatMatches(_plan.Source(), *source_stat) )
            return false;

        const auto destination_stat = host->Stat(_plan.DestinationPath(), VFSFlags::F_NoFollow);
        if( _plan.IsCaseOnlyRename() ) {
            return !*case_sensitive && host->IsNativeFS() && destination_stat &&
                   SourceStatMatches(_plan.Source(), *destination_stat) &&
                   StatsIdentifySameNativeEntry(*source_stat, *destination_stat);
        }
        return MissingAtPath(host, destination_stat);
    } catch( ... ) {
        return false;
    }
}

std::shared_ptr<nc::ops::Copying> MakeInlineRenameOperation(const InlineRenamePlan &_plan) noexcept
{
    try {
        nc::ops::CopyingOptions options;
        options.docopy = false;
        options.exist_behavior = nc::ops::CopyingOptions::ExistBehavior::Stop;
        options.destination_path_interpretation =
            nc::ops::CopyingOptions::DestinationPathInterpretation::ExactItem;

        auto operation = std::make_shared<nc::ops::Copying>(
            std::vector<VFSListingItem>{_plan.Source()}, _plan.DestinationPath(), _plan.Host(), options);
        operation->SetRuntimePreflightValidator([plan = _plan] { return RevalidateInlineRenameRuntime(plan); });
        return operation;
    } catch( ... ) {
        return {};
    }
}

} // namespace nc::panel::actions
