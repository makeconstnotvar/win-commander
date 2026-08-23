// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerPalette.h"
#include <Utility/HexadecimalColor.h>

#include <string_view>

namespace nc::explorer {

namespace {

// Builds one token. The two concrete colours are created once and captured; the provider block only
// picks between them. Both accessibility high-contrast appearances are listed so each matches its
// own base rather than falling back to Aqua. bestMatch may return nil, which correctly yields light.
NSColor *MakeToken(NSString *_name, std::string_view _light, std::string_view _dark)
{
    NSColor *const light = [NSColor colorWithHexString:_light];
    NSColor *const dark = [NSColor colorWithHexString:_dark];
    return [NSColor colorWithName:_name
                  dynamicProvider:^NSColor *(NSAppearance *_appearance) {
                    NSAppearanceName const matched = [_appearance bestMatchFromAppearancesWithNames:@[
                        NSAppearanceNameAqua,
                        NSAppearanceNameDarkAqua,
                        NSAppearanceNameAccessibilityHighContrastAqua,
                        NSAppearanceNameAccessibilityHighContrastDarkAqua,
                    ]];
                    const bool is_dark =
                        [matched isEqualToString:NSAppearanceNameDarkAqua] ||
                        [matched isEqualToString:NSAppearanceNameAccessibilityHighContrastDarkAqua];
                    return is_dark ? dark : light;
                  }];
}

} // namespace

// Reference planes: light content #FFFFFF (255), dark content #1E1E1E (30) - what
// NSColor.controlBackgroundColor resolves to, which is what the rows and the scroll view already
// use. Every value below is <plane> -/+ the same offset. Re-derive, do not eyeball.

NSColor *ChromeFillColor() // -17 / +17
{
    static NSColor *const c = MakeToken(@"nc.explorer.chrome.fill", "#ECEEF1", "#2F2F31");
    return c;
}

NSColor *ChromeDividerColor() // chrome -22 / chrome +22
{
    static NSColor *const c = MakeToken(@"nc.explorer.chrome.divider", "#D8DBE1", "#45464A");
    return c;
}

NSColor *CommandBarFillColor() // -7 / +7
{
    static NSColor *const c = MakeToken(@"nc.explorer.commandbar.fill", "#F7F8FA", "#252527");
    return c;
}

NSColor *WorkspaceFillColor() // -10 / +10
{
    static NSColor *const c = MakeToken(@"nc.explorer.workspace.fill", "#F4F5F7", "#28282A");
    return c;
}

NSColor *TableHeaderFillColor() // -6 / +6
{
    static NSColor *const c = MakeToken(@"nc.explorer.tableheader.fill", "#F8F9FB", "#242426");
    return c;
}

NSColor *TableHeaderDividerColor() // header -23 / header +23
{
    static NSColor *const c = MakeToken(@"nc.explorer.tableheader.divider", "#DEE2E8", "#3A3B3F");
    return c;
}

NSColor *RowDividerColor() // -15 / +15
{
    static NSColor *const c = MakeToken(@"nc.explorer.row.divider", "#EEF0F3", "#2C2D2F");
    return c;
}

} // namespace nc::explorer
