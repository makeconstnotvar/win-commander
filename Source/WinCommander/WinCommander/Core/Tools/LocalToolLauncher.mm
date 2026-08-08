// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "LocalToolLauncher.h"

#include <Utility/StringExtras.h>

#include <Cocoa/Cocoa.h>

#include <memory>
#include <utility>

namespace nc::core {

namespace {

/** The application, whether it was named by bundle identifier or by path. */
NSURL *ResolveApplication(const std::string &_application)
{
    if( _application.empty() )
        return nil;
    if( _application.front() == '/' ) {
        NSURL *const url = [NSURL fileURLWithPath:[NSString stringWithUTF8StdString:_application]];
        // Asked for explicitly: a path that is not there must not reach Launch Services, which would
        // report it as a launch failure rather than as the missing application it is.
        return [url checkResourceIsReachableAndReturnError:nil] ? url : nil;
    }
    return [NSWorkspace.sharedWorkspace
        URLForApplicationWithBundleIdentifier:[NSString stringWithUTF8StdString:_application]];
}

} // namespace

void PerformLocalToolLaunch(const LocalToolLaunchRequest &_request, LocalToolLaunchCompletion _completion)
{
    NSURL *const application = ResolveApplication(_request.application);
    if( application == nil ) {
        // Reported straight away: this needs no launch to discover, and the user can act on it.
        if( _completion )
            _completion(LocalToolLaunchOutcome::ApplicationMissing);
        return;
    }

    NSMutableArray<NSURL *> *const documents = [NSMutableArray arrayWithCapacity:_request.documents.size()];
    for( const std::string &document : _request.documents )
        [documents addObject:[NSURL fileURLWithPath:[NSString stringWithUTF8StdString:document]]];

    // With nothing selected, the directory itself is what the tool is opened on - which is what makes
    // "open terminal here" land in the folder the user is looking at.
    if( documents.count == 0 && !_request.working_directory.empty() ) {
        [documents addObject:[NSURL fileURLWithPath:[NSString stringWithUTF8StdString:_request.working_directory]
                                        isDirectory:YES]];
    }

    NSWorkspaceOpenConfiguration *const configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;

    // Deliberately not waited for. An application can take many seconds to start, and blocking for
    // it would freeze the window that asked - the same stall an unresponsive network mount must not
    // be allowed to cause.
    const auto completion = std::make_shared<LocalToolLaunchCompletion>(std::move(_completion));
    [NSWorkspace.sharedWorkspace openURLs:documents
                     withApplicationAtURL:application
                            configuration:configuration
                        completionHandler:^(NSRunningApplication *_running, NSError *_error) {
                          if( *completion ) {
                              (*completion)(_running != nil && _error == nil ? LocalToolLaunchOutcome::Started
                                                                            : LocalToolLaunchOutcome::LaunchFailed);
                          }
                        }];
}

} // namespace nc::core
