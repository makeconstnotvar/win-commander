// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ExplorerDirectSearchBackend.h"

#include <Cocoa/Cocoa.h>
#include <WinCommander/Core/Search/SearchPlanning.h>

#include <functional>
#include <memory>
#include <optional>

@class PanelController;

namespace nc::panel {
struct DirectoryChangeRequest;
}

namespace nc::explorer {

struct ExplorerSearchPanelContent final {
    core::PaneId pane_id;
    VFSListingPtr listing;
    uint64_t data_generation = 0;
    VFSHostPtr uniform_host;
    std::string uniform_directory;
    VFSListingItem focused_item;
};

/** Testable weak panel boundary. Every method is invoked on the main queue. */
class ExplorerSearchPanelAccess
{
public:
    virtual ~ExplorerSearchPanelAccess() = default;
    [[nodiscard]] virtual std::optional<ExplorerSearchPanelContent> Capture() const = 0;
    virtual void CommitListing(const VFSListingPtr &_listing) = 0;
    [[nodiscard]] virtual bool SubmitReveal(std::shared_ptr<panel::DirectoryChangeRequest> _request) = 0;
};

using ExplorerSearchBackendProvider =
    std::function<std::shared_ptr<ExplorerSearchBackend>(core::SearchBackendKind)>;
using ExplorerSearchSnapshotHandler = std::function<void(std::optional<core::SearchSnapshot>)>;

} // namespace nc::explorer

/**
 * Main-queue reducer/controller for one exact Explorer pane Search Mode.
 * Backend callbacks carry owned values and are fenced by run, pane and content identity.
 */
@interface ExplorerSearchController : NSObject

- (instancetype)initWithPanel:(PanelController *)_panel
                       paneId:(nc::core::PaneId)_pane_id
              backendProvider:(nc::explorer::ExplorerSearchBackendProvider)_backend_provider
               snapshotHandler:(nc::explorer::ExplorerSearchSnapshotHandler)_snapshot_handler;

/** Dependency-injection initializer used by focused controller tests. */
- (instancetype)initWithPanelAccess:(std::shared_ptr<nc::explorer::ExplorerSearchPanelAccess>)_panel_access
                             paneId:(nc::core::PaneId)_pane_id
                    backendProvider:(nc::explorer::ExplorerSearchBackendProvider)_backend_provider
                     snapshotHandler:(nc::explorer::ExplorerSearchSnapshotHandler)_snapshot_handler;

/** Captures one exact uniform origin and the planning facts used by every rerun in this presentation. */
- (BOOL)presentWithPlanningFacts:(nc::core::SearchPlanningFacts)_facts;

/** Also publishes an owned Idle draft; empty criteria are valid until Start is requested. */
- (BOOL)presentWithPlanningFacts:(nc::core::SearchPlanningFacts)_facts
                  initialRequest:(nc::core::SearchRequest)_initial_request;

/** Plans and starts against the retained origin while fencing the currently visible panel content. */
- (BOOL)startSearch:(nc::core::SearchRequest)_request;

/** Starts the owned draft previously published by Present. */
- (BOOL)startPresentedDraft;

/** Non-blocking on main; the stopped backend is waited and destroyed by a background reaper. */
- (void)cancel;

/** Cancels an active run and closes the pane's Search Mode after external navigation or refresh. */
- (void)invalidateForExternalContentChange;

/** Keeps the controller-owned committed result listing; closes on every other content identity. */
- (void)synchronizeExternalContentChange;

/** Closes Search Mode while retaining no origin or backend authority. */
- (void)close;

/** Reveals the exact focused item from the controller's last committed result listing. */
- (BOOL)canRevealFocusedResult;
- (BOOL)revealFocusedResult;

- (std::optional<nc::core::SearchSnapshot>)snapshot;
- (BOOL)isPresented;

@end
