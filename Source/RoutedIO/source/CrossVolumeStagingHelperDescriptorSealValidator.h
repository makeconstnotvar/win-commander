// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingXPCCodec.h"

#include <cstdint>
#include <expected>
#include <utility>

namespace nc::routedio::cross_volume_staging::helper {

/** Helper-local proof that the two decoded descriptor rights still match the complete reviewed scalar claims. */
enum class BeginDescriptorValidationError : uint8_t {
    InvalidRequest,
    SourceStale,
    DestinationParentStale,
    HelperFailure,
};

class LeaseStore;

/**
 * Move-only admission proof.  The sole public producer validates the owned pair of decoded descriptors before this
 * type can reach the future lease store; it does not select a root, create an artifact or inspect a user pathname.
 */
class ValidatedBegin final
{
public:
    ValidatedBegin(const ValidatedBegin &) = delete;
    ValidatedBegin &operator=(const ValidatedBegin &) = delete;
    ValidatedBegin(ValidatedBegin &&_other) noexcept;
    ValidatedBegin &operator=(ValidatedBegin &&_other) noexcept;

    [[nodiscard]] const BeginRequest &Request() const noexcept { return m_Begin.request; }

private:
    explicit ValidatedBegin(xpc_codec::DecodedBegin _begin) noexcept;

    [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept { return m_Valid; }

    int m_CreatorPID{-1};
    xpc_codec::DecodedBegin m_Begin;
    bool m_Valid{true};

    friend class LeaseStore;
    friend std::expected<ValidatedBegin, BeginDescriptorValidationError>
    ValidateBeginDescriptors(xpc_codec::DecodedBegin _begin) noexcept;
};

/** Consumes decoded descriptor rights and returns admission proof only after exact descriptor and scalar-seal checks. */
[[nodiscard]] std::expected<ValidatedBegin, BeginDescriptorValidationError>
ValidateBeginDescriptors(xpc_codec::DecodedBegin _begin) noexcept;

} // namespace nc::routedio::cross_volume_staging::helper
