// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationPlan.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace nc::ops {

enum class OperationPlanCodecErrorCode : uint8_t {
    MalformedJSON,
    RootNotObject,
    UnexpectedMember,
    MissingMember,
    DuplicateMember,
    InvalidMemberType,
    UnsupportedSchemaVersion,
    InvalidEnumToken,
    InvalidBase64,
    ResourceLimitExceeded,
    TimestampOutOfRange,
    TimestampNotRepresentable,
    PlanValidationFailed
};

struct OperationPlanCodecError final {
    OperationPlanCodecErrorCode code;
    std::optional<OperationPlanValidationError> plan_validation_error;

    bool operator==(const OperationPlanCodecError &) const = default;
};

/**
 * Pure, versioned JSON codec for the immutable structural OperationPlan value.
 *
 * The codec does not persist, review, authorize, bind, enqueue, or execute a plan. Opaque plan/provider/path
 * strings are represented as canonical base64 so valid non-UTF-8 filesystem bytes round-trip losslessly.
 */
class OperationPlanCodec final
{
public:
    static constexpr uint32_t SchemaVersion = 1;
    static constexpr size_t MaxSources = 100'000;
    static constexpr size_t MaxOpaqueFieldBytes = 1 * 1024 * 1024;
    static constexpr size_t MaxDecodedOpaqueBytes = 64 * 1024 * 1024;
    static constexpr size_t MaxJSONBytes = 96 * 1024 * 1024;

    [[nodiscard]] static std::expected<std::string, OperationPlanCodecError> Encode(const OperationPlan &_plan);
    [[nodiscard]] static std::expected<OperationPlan, OperationPlanCodecError> Decode(std::string_view _json);
};

} // namespace nc::ops
