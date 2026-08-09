// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NativeCloudItemFacts.h"

#include <Utility/StringExtras.h>

#include <Foundation/Foundation.h>

namespace nc::core {

namespace {

/** A boolean resource value, or false when the system will not say. */
bool BoolResource(NSURL *_url, NSURLResourceKey _key)
{
    id value = nil;
    // Resource values are metadata the system already holds. Nothing here reads content, which is
    // what would fetch a placeholder.
    if( ![_url getResourceValue:&value forKey:_key error:nil] )
        return false;
    NSNumber *const number = [value isKindOfClass:NSNumber.class] ? value : nil;
    return number.boolValue;
}

} // namespace

NativeCloudProbe ProbeNativeCloudItem(const std::string &_path)
{
    NativeCloudProbe probe;
    if( _path.empty() )
        return probe;

    NSURL *const url = [NSURL fileURLWithPath:[NSString stringWithUTF8StdString:_path]];
    if( url == nil )
        return probe;

    // Asked first and on its own: everything below is meaningless outside a container, and an item
    // that is not in one must come back as plainly not cloud rather than as a half-filled answer.
    if( !BoolResource(url, NSURLIsUbiquitousItemKey) )
        return probe;
    probe.in_cloud_container = true;

    id status = nil;
    if( [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil] ) {
        NSString *const text = [status isKindOfClass:NSString.class] ? status : nil;
        // "Not downloaded" is exactly the placeholder case. Anything else - current, or stale but
        // present - means the bytes are here.
        probe.is_dataless_placeholder = [text isEqualToString:NSURLUbiquitousItemDownloadingStatusNotDownloaded];
    }

    probe.download_in_progress = BoolResource(url, NSURLUbiquitousItemIsDownloadingKey);
    probe.upload_in_progress = BoolResource(url, NSURLUbiquitousItemIsUploadingKey);
    probe.has_conflict = BoolResource(url, NSURLUbiquitousItemHasUnresolvedConflictsKey);
    // The deployment target is 11.0 and this key arrived in 11.3, so it is asked for only where it
    // exists. Absent it, an excluded item reads as an ordinary synced one - a milder wrong answer
    // than refusing to say anything about the item at all.
    if( @available(macOS 11.3, *) )
        probe.excluded_from_sync = BoolResource(url, NSURLUbiquitousItemIsExcludedFromSyncKey);
    return probe;
}

} // namespace nc::core
