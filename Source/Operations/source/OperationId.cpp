// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationId.h"

#include <charconv>

namespace nc::ops {

std::optional<OperationId> OperationId::Parse(const std::string_view _serialized) noexcept
{
    constexpr std::string_view prefix{"op-"};
    if( !_serialized.starts_with(prefix) || _serialized.size() == prefix.size() )
        return std::nullopt;

    uint64_t sequence = 0;
    const auto first = _serialized.data() + prefix.size();
    const auto last = _serialized.data() + _serialized.size();
    if( *first == '0' )
        return std::nullopt;
    const auto [parsed_until, error] = std::from_chars(first, last, sequence);
    if( error != std::errc{} || parsed_until != last || sequence == 0 )
        return std::nullopt;
    return OperationId{sequence};
}

std::string OperationId::ToString() const
{
    return "op-" + std::to_string(m_Sequence);
}

} // namespace nc::ops
