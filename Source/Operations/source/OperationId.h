// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nc::ops {

class OperationCenterModel;
class OperationJournal;

/**
 * Opaque identity of one execution record.
 *
 * It is deliberately distinct from OperationPlanId. The canonical textual form is
 * `op-<positive-decimal-sequence>` and is suitable for the durable record codec.
 */
class OperationId final
{
public:
    OperationId() = delete;

    [[nodiscard]] static std::optional<OperationId> Parse(std::string_view _serialized) noexcept;
    [[nodiscard]] std::string ToString() const;
    bool operator==(const OperationId &) const noexcept = default;

private:
    explicit constexpr OperationId(const uint64_t _sequence) noexcept : m_Sequence(_sequence) {}

    uint64_t m_Sequence;

    friend class OperationCenterModel;
    friend class OperationJournal;
};

} // namespace nc::ops
