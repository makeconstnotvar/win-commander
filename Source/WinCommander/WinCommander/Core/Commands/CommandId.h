// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <string>
#include <string_view>

namespace nc::core {

class CommandId final
{
public:
    CommandId() = default;
    explicit CommandId(std::string_view _value) : m_Value(_value) {}

    [[nodiscard]] std::string_view Value() const noexcept { return m_Value; }
    [[nodiscard]] bool IsValid() const noexcept { return IsValidValue(m_Value); }

    friend bool operator==(const CommandId &, const CommandId &) = default;

private:
    [[nodiscard]] static bool IsValidValue(std::string_view _value) noexcept
    {
        if( _value.empty() )
            return false;

        bool expecting_segment_start = true;
        for( const unsigned char character : _value ) {
            if( character == '.' ) {
                if( expecting_segment_start )
                    return false;
                expecting_segment_start = true;
                continue;
            }

            const bool is_alpha = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z');
            const bool is_digit = character >= '0' && character <= '9';
            if( expecting_segment_start ) {
                if( !is_alpha )
                    return false;
                expecting_segment_start = false;
            }
            else if( !is_alpha && !is_digit ) {
                return false;
            }
        }

        return !expecting_segment_start;
    }

    std::string m_Value;
};

} // namespace nc::core
