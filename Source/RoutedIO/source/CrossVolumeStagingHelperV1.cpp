// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
// Isolated V1 staging-helper security boundary.  It grants one-use descriptor leases but no staging or publication
// authority yet.
#include "CrossVolumeStagingHelperDescriptorSealValidator.h"
#include "CrossVolumeStagingHelperLeaseStore.h"

#include <Security/SecCode.h>
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdint>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

namespace protocol = nc::routedio::cross_volume_staging;
namespace codec = protocol::xpc_codec;
namespace helper = protocol::helper;

namespace {

constexpr char kServiceName[] = "com.wincommander.App.CrossVolumeStagingHelperV1";
constexpr char kClientRequirement[] =
    "identifier com.wincommander.App and "
    "anchor apple generic and certificate leaf[subject.OU] = \"AC5SJT236H\"";

bool HasRuntimeHardening(SecCodeRef _code) noexcept
{
    CFDictionaryRef signing_information = nullptr;
    if( SecCodeCopySigningInformation(_code, kSecCSDynamicInformation, &signing_information) != errSecSuccess ||
        signing_information == nullptr )
        return false;

    const CFNumberRef status = static_cast<CFNumberRef>(
        CFDictionaryGetValue(signing_information, kSecCodeInfoStatus));
    int flags = 0;
    const bool hardened = status != nullptr && CFGetTypeID(status) == CFNumberGetTypeID() &&
                          CFNumberGetValue(status, kCFNumberIntType, &flags) &&
                          (flags & kSecCodeSignatureRuntime) != 0;
    CFRelease(signing_information);
    return hardened;
}

bool ValidateLegacyPeer(const pid_t _pid) noexcept
{
    if( _pid <= 0 )
        return false;

    const CFNumberRef pid_value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &_pid);
    if( pid_value == nullptr )
        return false;
    const void *keys[] = {kSecGuestAttributePid};
    const void *values[] = {pid_value};
    const CFDictionaryRef attributes = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(pid_value);
    if( attributes == nullptr )
        return false;

    SecCodeRef code = nullptr;
    const OSStatus copy_status = SecCodeCopyGuestWithAttributes(nullptr, attributes, kSecCSDefaultFlags, &code);
    CFRelease(attributes);
    if( copy_status != errSecSuccess || code == nullptr )
        return false;

    const CFStringRef requirement_string = CFStringCreateWithCString(
        kCFAllocatorDefault, kClientRequirement, kCFStringEncodingUTF8);
    if( requirement_string == nullptr ) {
        CFRelease(code);
        return false;
    }
    SecRequirementRef requirement = nullptr;
    const OSStatus requirement_status = SecRequirementCreateWithString(requirement_string, kSecCSDefaultFlags, &requirement);
    CFRelease(requirement_string);
    if( requirement_status != errSecSuccess || requirement == nullptr ) {
        CFRelease(code);
        return false;
    }

    const OSStatus validity_status = SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement);
    const bool hardened = validity_status == errSecSuccess && HasRuntimeHardening(code);
    CFRelease(requirement);
    CFRelease(code);
    return hardened;
}

bool AuthenticatePeer(xpc_connection_t _peer) noexcept
{
    const pid_t pid = xpc_connection_get_pid(_peer);
    if( !ValidateLegacyPeer(pid) )
        return false;

    if( __builtin_available(macOS 12.0, *) )
        return xpc_connection_set_peer_code_signing_requirement(_peer, kClientRequirement) == 0;
    return true;
}

bool SendBeginResult(xpc_object_t _event, const protocol::BeginResult &_result) noexcept
{
    xpc_connection_t remote = xpc_dictionary_get_remote_connection(_event);
    if( remote == nullptr )
        return false;
    xpc_object_t reply = xpc_dictionary_create_reply(_event);
    if( reply == nullptr )
        return false;
    const auto populated = codec::PopulateBeginResultReply(reply, _result);
    if( populated )
        xpc_connection_send_message(remote, reply);
    xpc_release(reply);
    return populated.has_value();
}

bool SendCompletionResult(xpc_object_t _event, const protocol::CompletionResult &_result) noexcept
{
    xpc_connection_t remote = xpc_dictionary_get_remote_connection(_event);
    if( remote == nullptr )
        return false;
    xpc_object_t reply = xpc_dictionary_create_reply(_event);
    if( reply == nullptr )
        return false;
    const auto populated = codec::PopulateCompletionResultReply(reply, _result);
    if( populated )
        xpc_connection_send_message(remote, reply);
    xpc_release(reply);
    return populated.has_value();
}

protocol::BeginFailure BeginFailureFor(const helper::BeginDescriptorValidationError _error) noexcept
{
    switch( _error ) {
        case helper::BeginDescriptorValidationError::InvalidRequest:
            return protocol::BeginFailure::InvalidRequest;
        case helper::BeginDescriptorValidationError::SourceStale:
            return protocol::BeginFailure::SourceStale;
        case helper::BeginDescriptorValidationError::DestinationParentStale:
            return protocol::BeginFailure::DestinationParentStale;
        case helper::BeginDescriptorValidationError::HelperFailure:
            return protocol::BeginFailure::HelperFailure;
    }
    return protocol::BeginFailure::HelperFailure;
}

helper::OwnerID OwnerFor(const xpc_connection_t _peer) noexcept
{
    return static_cast<helper::OwnerID>(reinterpret_cast<uintptr_t>(_peer));
}

void DisconnectPeer(xpc_connection_t _peer, helper::LeaseLifecycle &_lifecycle, const helper::OwnerID _owner) noexcept
{
    (void)_lifecycle.RevokeOwner(_owner);
    xpc_connection_cancel(_peer);
}

void Dispatch(xpc_connection_t _peer,
              xpc_object_t _event,
              helper::LeaseLifecycle &_lifecycle,
              const helper::OwnerID _owner) noexcept
{
    const auto kind = codec::DecodeRequestKind(_event);
    if( !kind ) {
        DisconnectPeer(_peer, _lifecycle, _owner);
        return;
    }

    switch( *kind ) {
        case codec::RequestKind::Begin: {
            auto begin = codec::DecodeBegin(_event);
            if( !begin ) {
                DisconnectPeer(_peer, _lifecycle, _owner);
                return;
            }
            const protocol::Header header = begin->request.header;
            auto validated = helper::ValidateBeginDescriptors(std::move(*begin));
            if( !validated ) {
                if( !SendBeginResult(_event,
                                      {.header = header,
                                       .disposition = protocol::BeginDisposition::Rejected,
                                       .failure = BeginFailureFor(validated.error()),
                                       .lease = {.header = header}}) )
                    DisconnectPeer(_peer, _lifecycle, _owner);
                return;
            }
            if( !SendBeginResult(_event, _lifecycle.Begin(_owner, std::move(*validated))) )
                DisconnectPeer(_peer, _lifecycle, _owner);
            return;
        }
        case codec::RequestKind::Commit: {
            const auto commit = codec::DecodeCommit(_event);
            if( !commit ) {
                DisconnectPeer(_peer, _lifecycle, _owner);
                return;
            }
            if( !SendCompletionResult(_event, _lifecycle.Commit(_owner, *commit)) )
                DisconnectPeer(_peer, _lifecycle, _owner);
            return;
        }
        case codec::RequestKind::Abort: {
            const auto abort = codec::DecodeAbort(_event);
            if( !abort ) {
                DisconnectPeer(_peer, _lifecycle, _owner);
                return;
            }
            if( !SendCompletionResult(_event, _lifecycle.Abort(_owner, *abort)) )
                DisconnectPeer(_peer, _lifecycle, _owner);
            return;
        }
    }
    DisconnectPeer(_peer, _lifecycle, _owner);
}

void AcceptPeer(xpc_connection_t _peer, helper::LeaseLifecycle &_lifecycle) noexcept
{
    if( !AuthenticatePeer(_peer) ) {
        xpc_connection_cancel(_peer);
        return;
    }
    const helper::OwnerID owner = OwnerFor(_peer);
    if( owner == 0 ) {
        xpc_connection_cancel(_peer);
        return;
    }

    xpc_connection_set_event_handler(_peer, ^(xpc_object_t event) {
      if( xpc_get_type(event) != XPC_TYPE_DICTIONARY ) {
          DisconnectPeer(_peer, _lifecycle, owner);
          return;
      }
      Dispatch(_peer, event, _lifecycle, owner);
    });
    xpc_connection_resume(_peer);
}

} // namespace

int main()
{
    if( geteuid() != 0 )
        return EXIT_FAILURE;

    umask(077);
    // The listener never returns from dispatch_main().  Deliberately retain this process-wide helper state instead of
    // registering exit-time destructors, which would race service teardown after the XPC runtime begins shutdown.
    auto *const leases = new helper::LeaseStore;
    auto *const lease_lifecycle = new helper::LeaseLifecycle{*leases};
    xpc_connection_t service = xpc_connection_create_mach_service(
        kServiceName, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if( service == nullptr )
        return EXIT_FAILURE;

    xpc_connection_set_event_handler(service, ^(xpc_object_t object) {
      if( xpc_get_type(object) == XPC_TYPE_CONNECTION )
          AcceptPeer(static_cast<xpc_connection_t>(object), *lease_lifecycle);
    });
    xpc_connection_resume(service);
    dispatch_main();
}
