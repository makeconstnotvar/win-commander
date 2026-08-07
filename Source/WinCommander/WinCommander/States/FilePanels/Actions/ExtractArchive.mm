// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExtractArchive.h"

#include "../MainWindowFilePanelState.h"
#include "../PanelAux.h"
#include "../PanelController.h"
#include "../../MainWindowController.h"
#include <WinCommander/Core/Operations/ArchiveExtractionManifest.h>
#include <WinCommander/GeneralUI/AskForPasswordWindowController.h>
#include <Base/dispatch_cpp.h>
#include <Operations/Copying.h>
#include <Panel/PanelData.h>
#include <Utility/PathManip.h>
#include <VFS/ProviderCapabilities.h>
#include <VFS/VFSArchiveProxy.h>
#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nc::panel::actions {

std::optional<ArchiveExtractionSourceIdentity>
ArchiveExtractionSourceIdentity::Capture(const VFSListingItem &_source) noexcept
{
    try {
        if( !_source || !_source.Host() )
            return std::nullopt;
        const auto stat = _source.Host()->Stat(_source.Path(), VFSFlags::F_NoFollow);
        if( !stat || !stat->meaning.mode || !stat->meaning.inode || !stat->meaning.size || !stat->meaning.mtime ||
            !S_ISREG(stat->mode) || S_ISLNK(stat->mode) ) {
            return std::nullopt;
        }
        return ArchiveExtractionSourceIdentity{
            .inode = stat->inode,
            .size = stat->size,
            .modification_seconds = stat->mtime.tv_sec,
            .modification_nanoseconds = stat->mtime.tv_nsec,
        };
    } catch( ... ) {
        return std::nullopt;
    }
}

bool ArchiveExtractionSourceIdentity::Matches(const VFSListingItem &_source) const noexcept
{
    const auto current = Capture(_source);
    return current && *current == *this;
}

namespace {

enum class PlanFailureKind : uint8_t {
    Cancelled,
    PasswordCancelled,
    ArchiveOpenFailed,
    ArchiveReadFailed,
    EmptyArchive,
    ManifestRejected,
    DestinationRejected
};

struct PlanFailure final {
    PlanFailureKind kind{PlanFailureKind::ArchiveReadFailed};
    std::string detail;
};

struct ExtractionPlan final {
    std::vector<VFSListingItem> root_items;
    std::shared_ptr<const core::ArchiveExtractionManifest> manifest;
};

struct PendingDirectory final {
    std::string path;
    std::vector<std::string> components;
    size_t component_bytes{0};
};

struct NativeDestinationDirectorySeal final {
    int32_t device{0};
    uint64_t inode{0};
    timespec birth_time{.tv_sec = 0, .tv_nsec = 0};

    bool operator==(const NativeDestinationDirectorySeal &_rhs) const noexcept
    {
        return device == _rhs.device && inode == _rhs.inode && birth_time.tv_sec == _rhs.birth_time.tv_sec &&
               birth_time.tv_nsec == _rhs.birth_time.tv_nsec;
    }
};

struct ExtractionSubmissionCapture final {
    VFSListingItem source;
    ArchiveExtractionSourceIdentity source_identity;
    VFSListingPtr source_listing;
    unsigned long source_generation{0};
    VFSHostPtr destination_host;
    std::string destination;
    NativeDestinationDirectorySeal destination_seal;
    bool case_sensitive{true};
    NCMainWindowController *__strong window_controller{nil};
};

[[nodiscard]] bool IsSupportedArchiveItem(const VFSListingItem &_item) noexcept
{
    return _item && !_item.IsDotDot() && !_item.IsDir() && !_item.IsSymlink() && _item.IsReg() &&
           _item.HasExtension() && IsExtensionInArchivesWhitelist(_item.Extension());
}

[[nodiscard]] bool DestinationSupportsExtraction(const VFSHostPtr &_host, const std::string &_path) noexcept
{
    try {
        if( !_host || !_host->IsWritableAtPath(_path) )
            return false;
        const vfs::ProviderCapabilities capabilities = vfs::ProviderCapabilitiesResolver::Resolve(*_host, _path);
        return capabilities.is_native && capabilities.can_write && capabilities.can_create_file &&
               capabilities.can_create_folder && capabilities.can_create_symlink;
    } catch( ... ) {
        return false;
    }
}

[[nodiscard]] std::optional<NativeDestinationDirectorySeal>
CaptureNativeDestinationDirectorySeal(const VFSHostPtr &_host, const std::string &_path) noexcept
{
    try {
        if( !_host || !_host->IsNativeFS() )
            return std::nullopt;
        const auto stat = _host->Stat(_path, VFSFlags::F_NoFollow);
        if( !stat || !stat->meaning.mode || !stat->meaning.dev || !stat->meaning.inode || !stat->meaning.btime ||
            !S_ISDIR(stat->mode) || S_ISLNK(stat->mode) ) {
            return std::nullopt;
        }
        return NativeDestinationDirectorySeal{
            .device = stat->dev,
            .inode = stat->inode,
            .birth_time = stat->btime,
        };
    } catch( ... ) {
        return std::nullopt;
    }
}

[[nodiscard]] bool NativeDestinationDirectoryMatchesSeal(const VFSHostPtr &_host,
                                                         const std::string &_path,
                                                         const NativeDestinationDirectorySeal &_seal) noexcept
{
    const auto current = CaptureNativeDestinationDirectorySeal(_host, _path);
    return current && *current == _seal;
}

[[nodiscard]] bool DestinationBindingMatches(const VFSHostPtr &_host,
                                             const std::string &_path,
                                             const NativeDestinationDirectorySeal &_seal,
                                             const bool _case_sensitive) noexcept
{
    try {
        return _host && _host->CaseSensitivityAtPath(_path) == std::optional{_case_sensitive} &&
               DestinationSupportsExtraction(_host, _path) &&
               NativeDestinationDirectoryMatchesSeal(_host, _path, _seal);
    } catch( ... ) {
        return false;
    }
}

[[nodiscard]] core::ArchiveExtractionEntryKind KindOf(const VFSListingItem &_item) noexcept
{
    if( _item.IsSymlink() )
        return core::ArchiveExtractionEntryKind::Symlink;
    if( _item.IsDir() )
        return core::ArchiveExtractionEntryKind::Directory;
    if( _item.IsReg() )
        return core::ArchiveExtractionEntryKind::RegularFile;
    return core::ArchiveExtractionEntryKind::Special;
}

[[nodiscard]] std::string ChildPath(std::string _parent, const std::string &_component)
{
    if( _parent.empty() || _parent.back() != '/' )
        _parent.push_back('/');
    _parent += _component;
    return _parent;
}

[[nodiscard]] std::optional<std::string> CaseInsensitiveIdentity(const std::string &_component)
{
    NSString *const source = [[NSString alloc] initWithBytes:_component.data()
                                                      length:_component.size()
                                                    encoding:NSUTF8StringEncoding];
    if( source == nil )
        return std::nullopt;
    NSString *const normalized = source.precomposedStringWithCanonicalMapping.lowercaseString;
    const char *const utf8 = normalized.UTF8String;
    if( utf8 == nullptr )
        return std::nullopt;
    return std::string{utf8};
}

[[nodiscard]] bool UnicodeNamespaceIsSafe(const std::vector<core::ArchiveExtractionMaterializedEntry> &_entries,
                                          const VFSCancelChecker &_cancelled)
{
    std::vector<core::ArchiveExtractionMaterializedEntry> normalized = _entries;
    for( core::ArchiveExtractionMaterializedEntry &entry : normalized ) {
        if( _cancelled && _cancelled() )
            return false;
        for( std::string &component : entry.components ) {
            auto identity = CaseInsensitiveIdentity(component);
            if( !identity )
                return false;
            component = std::move(*identity);
        }
    }
    return core::ArchiveExtractionManifest::Build(std::move(normalized), true).has_value();
}

[[nodiscard]] bool DestinationNamespaceIsSafe(const VFSHostPtr &_host,
                                              const std::string &_destination,
                                              const core::ArchiveExtractionManifest &_manifest,
                                              const NativeDestinationDirectorySeal &_destination_seal,
                                              const bool _case_sensitive,
                                              const VFSCancelChecker &_cancelled = {})
{
    try {
        if( _cancelled && _cancelled() )
            return false;
        if( !DestinationBindingMatches(_host, _destination, _destination_seal, _case_sensitive) ) {
            return false;
        }

        for( const core::ArchiveExtractionMaterializedEntry &entry : _manifest.Entries() ) {
            if( _cancelled && _cancelled() )
                return false;
            std::string path = _destination;
            for( size_t index = 0; index < entry.components.size(); ++index ) {
                if( _cancelled && _cancelled() )
                    return false;
                path = ChildPath(std::move(path), entry.components[index]);
                const auto stat = _host->Stat(path, VFSFlags::F_NoFollow);
                if( !stat ) {
                    if( _host->ClassifyError(stat.error()) == vfs::HostErrorKind::Missing )
                        continue;
                    return false;
                }
                if( !stat->meaning.mode )
                    return false;
                if( S_ISLNK(stat->mode) )
                    return false;
                if( index + 1 < entry.components.size() ) {
                    if( !S_ISDIR(stat->mode) )
                        return false;
                    continue;
                }

                switch( entry.kind ) {
                    case core::ArchiveExtractionEntryKind::RegularFile:
                        if( !S_ISREG(stat->mode) )
                            return false;
                        break;
                    case core::ArchiveExtractionEntryKind::Directory:
                        if( !S_ISDIR(stat->mode) )
                            return false;
                        break;
                    case core::ArchiveExtractionEntryKind::Symlink:
                    case core::ArchiveExtractionEntryKind::Special:
                        // Existing symlinks were rejected above. Replacing any other existing kind
                        // with an archive symlink, or materializing a special entry, is outside this
                        // bounded Extract Here contract.
                        return false;
                }
                if( !NativeDestinationDirectoryMatchesSeal(_host, _destination, _destination_seal) ) {
                    return false;
                }
            }
        }
        return NativeDestinationDirectoryMatchesSeal(_host, _destination, _destination_seal);
    } catch( ... ) {
        return false;
    }
}

[[nodiscard]] PlanFailure ArchiveOpenFailure(const vfs::ArchiveOpenFailure &_failure)
{
    using Kind = vfs::ArchiveOpenFailureKind;
    switch( _failure.kind ) {
        case Kind::Cancelled:
            return {.kind = PlanFailureKind::Cancelled};
        case Kind::PasswordCancelled:
            return {.kind = PlanFailureKind::PasswordCancelled};
        case Kind::PasswordRequired:
        case Kind::PasswordRejectedOrInvalidArchive:
        case Kind::InvalidOrUnsupportedArchive:
            return {.kind = PlanFailureKind::ArchiveOpenFailed,
                    .detail = _failure.primary_error.LocalizedFailureReason()};
        case Kind::SourceMissing:
        case Kind::SourcePermissionDenied:
        case Kind::SourceUnavailable:
        case Kind::ReadFailed:
            return {.kind = PlanFailureKind::ArchiveReadFailed,
                    .detail = _failure.primary_error.LocalizedFailureReason()};
    }
}

[[nodiscard]] std::expected<ExtractionPlan, PlanFailure> BuildExtractionPlan(const VFSListingItem &_source,
                                                                             const ArchiveExtractionSourceIdentity &_source_identity,
                                                                             const VFSHostPtr &_destination_host,
                                                                             const std::string &_destination,
                                                                             const NativeDestinationDirectorySeal &_destination_seal,
                                                                             const bool _case_sensitive,
                                                                             const VFSCancelChecker &_cancelled)
{
    if( _cancelled && _cancelled() )
        return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});
    if( !_source_identity.Matches(_source) ) {
        return std::unexpected(PlanFailure{
            .kind = PlanFailureKind::ArchiveReadFailed,
            .detail = "The archive source changed before it could be opened.",
        });
    }
    if( !DestinationBindingMatches(_destination_host, _destination, _destination_seal, _case_sensitive) ) {
        return std::unexpected(PlanFailure{.kind = PlanFailureKind::DestinationRejected});
    }

    auto password = [source = _source, source_identity = _source_identity]() -> std::optional<std::string> {
        std::string value;
        if( !RunAskForPasswordModalWindow(source.Filename(), value) || !source_identity.Matches(source) )
            return std::nullopt;
        return value;
    };
    auto archive =
        vfs::VFSArchiveProxy::OpenFileAsArchiveResult(_source.Path(), _source.Host(), std::move(password), _cancelled);
    if( !archive )
        return std::unexpected(ArchiveOpenFailure(archive.error()));
    if( !_source_identity.Matches(_source) ) {
        return std::unexpected(PlanFailure{
            .kind = PlanFailureKind::ArchiveReadFailed,
            .detail = "The archive source changed while it was being opened.",
        });
    }

    std::vector<VFSListingItem> root_items;
    std::vector<core::ArchiveExtractionMaterializedEntry> entries;
    std::vector<PendingDirectory> pending{{.path = "/", .components = {}, .component_bytes = 0}};
    ArchiveExtractionTraversalBudget traversal_budget;

    while( !pending.empty() ) {
        if( _cancelled && _cancelled() )
            return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});

        PendingDirectory directory = std::move(pending.back());
        pending.pop_back();
        const auto listing = (*archive)->FetchDirectoryListing(directory.path, VFSFlags::F_NoDotDot, _cancelled);
        if( !listing ) {
            if( (*archive)->ClassifyError(listing.error()) == vfs::HostErrorKind::Cancelled )
                return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});
            return std::unexpected(PlanFailure{.kind = PlanFailureKind::ArchiveReadFailed,
                                               .detail = listing.error().LocalizedFailureReason()});
        }

        for( const VFSListingItem &item : **listing ) {
            if( _cancelled && _cancelled() )
                return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});
            const size_t filename_bytes = item.Filename().size();
            const ArchiveExtractionTraversalBudget::Admission admission = traversal_budget.Admit(
                directory.components.size(), directory.component_bytes, filename_bytes);
            if( admission != ArchiveExtractionTraversalBudget::Admission::Accepted ) {
                return std::unexpected(PlanFailure{
                    .kind = PlanFailureKind::ManifestRejected,
                    .detail = admission == ArchiveExtractionTraversalBudget::Admission::DepthExceeded
                                  ? "The archive namespace exceeds the maximum extraction depth."
                                  : "The archive namespace exceeds the bounded extraction manifest.",
                });
            }
            const size_t entry_name_bytes = directory.component_bytes + filename_bytes;

            std::vector<std::string> components = directory.components;
            components.emplace_back(item.Filename());
            const core::ArchiveExtractionEntryKind kind = KindOf(item);
            entries.emplace_back(core::ArchiveExtractionMaterializedEntry{
                .components = components,
                .kind = kind,
            });

            if( directory.components.empty() )
                root_items.emplace_back(item);
            if( kind == core::ArchiveExtractionEntryKind::Directory )
                pending.emplace_back(PendingDirectory{
                    .path = item.Path(),
                    .components = std::move(components),
                    .component_bytes = entry_name_bytes,
                });
        }
    }

    if( root_items.empty() )
        return std::unexpected(PlanFailure{.kind = PlanFailureKind::EmptyArchive});

    if( !_case_sensitive && !UnicodeNamespaceIsSafe(entries, _cancelled) ) {
        if( _cancelled && _cancelled() )
            return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});
        return std::unexpected(PlanFailure{
            .kind = PlanFailureKind::ManifestRejected,
            .detail = "The archive contains colliding names for this case-insensitive destination.",
        });
    }

    if( _cancelled && _cancelled() )
        return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});

    auto manifest = core::ArchiveExtractionManifest::Build(
        std::move(entries), _case_sensitive, [host = _destination_host](const std::string_view _component) {
            return host->ValidateFilename(_component);
        });
    if( !manifest ) {
        return std::unexpected(PlanFailure{
            .kind = PlanFailureKind::ManifestRejected,
            .detail = "The archive namespace failed extraction validation.",
        });
    }

    auto sealed_manifest = std::make_shared<const core::ArchiveExtractionManifest>(std::move(*manifest));
    if( !DestinationNamespaceIsSafe(_destination_host,
                                    _destination,
                                    *sealed_manifest,
                                    _destination_seal,
                                    _case_sensitive,
                                    _cancelled) ) {
        if( _cancelled && _cancelled() )
            return std::unexpected(PlanFailure{.kind = PlanFailureKind::Cancelled});
        return std::unexpected(PlanFailure{
            .kind = PlanFailureKind::DestinationRejected,
            .detail = "The extraction destination contains an unsafe or unavailable path prefix.",
        });
    }

    return ExtractionPlan{
        .root_items = std::move(root_items),
        .manifest = std::move(sealed_manifest),
    };
}

void PresentPlanFailure(PanelController *_target, const PlanFailure &_failure)
{
    if( _failure.kind == PlanFailureKind::Cancelled || _failure.kind == PlanFailureKind::PasswordCancelled )
        return;
    if( !_target || !_target.mainWindowController ) {
        NSBeep();
        return;
    }

    NSString *message = nil;
    switch( _failure.kind ) {
        case PlanFailureKind::ArchiveOpenFailed:
            message = NSLocalizedString(@"commands.archive.extract.failure.invalidArchive", "Extract archive error");
            break;
        case PlanFailureKind::ArchiveReadFailed:
            message = NSLocalizedString(@"commands.archive.extract.failure.read", "Extract archive error");
            break;
        case PlanFailureKind::EmptyArchive:
            message = NSLocalizedString(@"commands.archive.extract.failure.empty", "Extract archive error");
            break;
        case PlanFailureKind::ManifestRejected:
            message = NSLocalizedString(@"commands.archive.extract.failure.manifest", "Extract archive error");
            break;
        case PlanFailureKind::DestinationRejected:
            message = NSLocalizedString(@"commands.archive.extract.failure.destination", "Extract archive error");
            break;
        case PlanFailureKind::Cancelled:
        case PlanFailureKind::PasswordCancelled:
            return;
    }

    NSAlert *const alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = message;
    if( !_failure.detail.empty() )
        alert.informativeText = [NSString stringWithUTF8String:_failure.detail.c_str()];
    [alert addButtonWithTitle:NSLocalizedString(@"OK", "Alert confirmation button")];
    [alert beginSheetModalForWindow:_target.mainWindowController.window completionHandler:nil];
}

} // namespace

ArchiveExtractionSubmissionResult EvaluateArchiveExtractionSubmission(const std::span<const VFSListingItem> _items,
                                                                      PanelController *_target)
{
    if( !_target )
        return ArchiveExtractionSubmissionResult::PaneUnavailable;
    if( !_target.mainWindowController )
        return ArchiveExtractionSubmissionResult::WindowUnavailable;
    if( _target.isDoingBackgroundLoading )
        return ArchiveExtractionSubmissionResult::Loading;
    if( _items.size() != 1 )
        return ArchiveExtractionSubmissionResult::SelectionUnavailable;
    if( _items.front().IsDotDot() )
        return ArchiveExtractionSubmissionResult::ParentEntryUnsupported;
    if( !IsSupportedArchiveItem(_items.front()) )
        return ArchiveExtractionSubmissionResult::SourceUnsupported;

    try {
        const VFSListingPtr listing = _target.data.ListingPtr();
        if( !_target.isUniform || !_target.vfs || !listing )
            return ArchiveExtractionSubmissionResult::DestinationUnavailable;
        const VFSListingItem &source = _items.front();
        if( !source.Host() || source.Listing().get() != listing.get() )
            return ArchiveExtractionSubmissionResult::StaleContext;
        if( !vfs::ProviderCapabilitiesResolver::Resolve(*source.Host(), source.Directory()).can_read )
            return ArchiveExtractionSubmissionResult::SourceUnreadable;
        if( !ArchiveExtractionSourceIdentity::Capture(source) )
            return ArchiveExtractionSubmissionResult::SourceUnreadable;

        const std::string destination = _target.currentDirectoryPath;
        if( !_target.vfs->IsWritableAtPath(destination) )
            return ArchiveExtractionSubmissionResult::DestinationReadOnly;
        if( !DestinationSupportsExtraction(_target.vfs, destination) )
            return ArchiveExtractionSubmissionResult::ProviderUnsupported;
        if( !_target.vfs->CaseSensitivityAtPath(destination) )
            return ArchiveExtractionSubmissionResult::CaseSensitivityUnavailable;
        if( !CaptureNativeDestinationDirectorySeal(_target.vfs, destination) )
            return ArchiveExtractionSubmissionResult::DestinationUnavailable;
    } catch( ... ) {
        return ArchiveExtractionSubmissionResult::DestinationUnavailable;
    }
    return ArchiveExtractionSubmissionResult::Submitted;
}

ArchiveExtractionSubmissionResult SubmitArchiveExtraction(const std::span<const VFSListingItem> _items,
                                                          PanelController *_target)
{
    const ArchiveExtractionSubmissionResult live = EvaluateArchiveExtractionSubmission(_items, _target);
    if( live != ArchiveExtractionSubmissionResult::Submitted )
        return live;

    std::optional<ExtractionSubmissionCapture> captured;
    try {
        const VFSListingItem source = _items.front();
        const auto source_identity = ArchiveExtractionSourceIdentity::Capture(source);
        if( !source_identity )
            return ArchiveExtractionSubmissionResult::SourceUnreadable;
        const VFSListingPtr source_listing = _target.data.ListingPtr();
        const unsigned long source_generation = _target.dataGeneration;
        const VFSHostPtr destination_host = _target.vfs;
        const std::string destination = _target.currentDirectoryPath;
        NCMainWindowController *const window_controller = _target.mainWindowController;
        const auto case_sensitive = destination_host ? destination_host->CaseSensitivityAtPath(destination) : std::nullopt;
        if( !case_sensitive )
            return ArchiveExtractionSubmissionResult::CaseSensitivityUnavailable;
        const auto destination_seal = CaptureNativeDestinationDirectorySeal(destination_host, destination);
        if( !destination_seal )
            return ArchiveExtractionSubmissionResult::StaleContext;

        const std::array<VFSListingItem, 1> exact_source{source};
        const ArchiveExtractionSubmissionResult rebound = EvaluateArchiveExtractionSubmission(exact_source, _target);
        if( rebound != ArchiveExtractionSubmissionResult::Submitted )
            return rebound;
        if( _target.mainWindowController != window_controller || _target.dataGeneration != source_generation ||
            _target.data.ListingPtr() != source_listing || source.Listing() != source_listing ||
            _target.vfs != destination_host || _target.currentDirectoryPath != destination ||
            destination_host->CaseSensitivityAtPath(destination) != case_sensitive ||
            !source_identity->Matches(source) ||
            !NativeDestinationDirectoryMatchesSeal(destination_host, destination, *destination_seal) ) {
            return ArchiveExtractionSubmissionResult::StaleContext;
        }

        captured.emplace(ExtractionSubmissionCapture{
            .source = source,
            .source_identity = *source_identity,
            .source_listing = source_listing,
            .source_generation = source_generation,
            .destination_host = destination_host,
            .destination = destination,
            .destination_seal = *destination_seal,
            .case_sensitive = *case_sensitive,
            .window_controller = window_controller,
        });
    } catch( ... ) {
        return ArchiveExtractionSubmissionResult::StaleContext;
    }

    const VFSListingItem source = captured->source;
    const ArchiveExtractionSourceIdentity source_identity = captured->source_identity;
    const VFSListingPtr source_listing = captured->source_listing;
    const unsigned long source_generation = captured->source_generation;
    const VFSHostPtr destination_host = captured->destination_host;
    const std::string destination = captured->destination;
    const NativeDestinationDirectorySeal destination_seal = captured->destination_seal;
    const bool case_sensitive = captured->case_sensitive;
    NCMainWindowController *const window_controller = captured->window_controller;
    __weak PanelController *weak_target = _target;
    __weak NCMainWindowController *weak_window = window_controller;
    std::shared_ptr<ActivityTicket> activity;
    try {
        activity = std::make_shared<ActivityTicket>([_target registerExtActivity]);
    } catch( ... ) {
        return ArchiveExtractionSubmissionResult::StaleContext;
    }

    auto task = [source,
                 source_identity,
                 source_listing,
                 source_generation,
                 destination_host,
                 destination,
                 destination_seal,
                 case_sensitive,
                 weak_target,
                 weak_window,
                 activity](const CancelableLoadingTaskContext &_context) mutable {
        auto plan = [&]() -> std::expected<ExtractionPlan, PlanFailure> {
            try {
                return BuildExtractionPlan(source,
                                           source_identity,
                                           destination_host,
                                           destination,
                                           destination_seal,
                                           case_sensitive,
                                           _context.is_cancelled);
            } catch( ... ) {
                return std::unexpected(PlanFailure{
                    .kind = PlanFailureKind::ArchiveReadFailed,
                    .detail = "The extraction plan could not be constructed.",
                });
            }
        }();
        if( !plan ) {
            _context.commit_on_main([weak_target, activity, failure = std::move(plan.error())] {
                (void)activity;
                PresentPlanFailure(static_cast<PanelController *>(weak_target), failure);
            });
            return;
        }

        _context.commit_on_main([source,
                                 source_identity,
                                 source_listing,
                                 source_generation,
                                 destination_host,
                                 destination,
                                 destination_seal,
                                 case_sensitive,
                                 weak_target,
                                 weak_window,
                                 activity,
                                 plan = std::move(*plan)]() mutable {
            (void)activity;
            PanelController *const target = weak_target;
            NCMainWindowController *const current_window = weak_window;
            if( !target || !current_window || target.mainWindowController != current_window ||
                target.dataGeneration != source_generation || target.data.ListingPtr() != source_listing ||
                target.vfs != destination_host || target.currentDirectoryPath != destination ||
                !DestinationBindingMatches(destination_host, destination, destination_seal, case_sensitive) ||
                !source_identity.Matches(source) ) {
                PresentPlanFailure(target, PlanFailure{.kind = PlanFailureKind::DestinationRejected});
                return;
            }

            const std::array<VFSListingItem, 1> exact_source{source};
            if( EvaluateArchiveExtractionSubmission(exact_source, target) !=
                ArchiveExtractionSubmissionResult::Submitted ) {
                PresentPlanFailure(target, PlanFailure{.kind = PlanFailureKind::DestinationRejected});
                return;
            }

            try {
                nc::ops::CopyingOptions options = MakeDefaultFileCopyOptions();
                options.docopy = true;
                options.preserve_symlinks = true;
                options.reject_final_component_symlinks = true;
                options.exist_behavior = nc::ops::CopyingOptions::ExistBehavior::Ask;
                options.destination_path_interpretation =
                    nc::ops::CopyingOptions::DestinationPathInterpretation::Directory;
                auto operation = std::make_shared<nc::ops::Copying>(
                    std::move(plan.root_items), destination, destination_host, options);
                operation->SetRuntimePreflightValidator(
                    [destination_host, destination, manifest = plan.manifest, destination_seal, case_sensitive] {
                        return DestinationNamespaceIsSafe(
                            destination_host, destination, *manifest, destination_seal, case_sensitive);
                    });

                __weak PanelController *weak_refresh_target = target;
                operation->ObserveUnticketed(
                    nc::ops::Operation::NotifyAboutCompletion, [weak_refresh_target, destination_host, destination] {
                        dispatch_to_main_queue([weak_refresh_target, destination_host, destination] {
                            if( PanelController *const panel = weak_refresh_target ) {
                                if( panel.vfs == destination_host && panel.currentDirectoryPath == destination )
                                    [panel refreshPanel];
                            }
                        });
                    });
                [current_window enqueueOperation:operation];
            } catch( ... ) {
                PresentPlanFailure(target, PlanFailure{.kind = PlanFailureKind::DestinationRejected});
            }
        });
    };

    try {
        [_target commitCancelableLoadingTask:std::move(task)];
    } catch( ... ) {
        return ArchiveExtractionSubmissionResult::StaleContext;
    }
    return ArchiveExtractionSubmissionResult::Submitted;
}

bool ExtractArchiveHere::Predicate(PanelController *_target) const
{
    if( !_target )
        return false;
    const auto items = _target.selectedEntriesOrFocusedEntry;
    return EvaluateArchiveExtractionSubmission(items, _target) == ArchiveExtractionSubmissionResult::Submitted;
}

void ExtractArchiveHere::Perform(PanelController *_target, id /*_sender*/) const
{
    if( !_target )
        return;
    const auto items = _target.selectedEntriesOrFocusedEntry;
    if( SubmitArchiveExtraction(items, _target) != ArchiveExtractionSubmissionResult::Submitted )
        NSBeep();
}

} // namespace nc::panel::actions
