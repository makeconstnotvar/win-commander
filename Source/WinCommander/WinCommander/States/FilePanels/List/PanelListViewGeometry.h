// Copyright (C) 2016-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

@class NSFont;

namespace nc::panel {

// Horizontal metrics of one list cell. The defaults reproduce the Commander presentation exactly;
// the Explorer presentation passes the FM-02 mockup's 12 / 12 / 8. Declared at namespace scope so
// it can serve as a defaulted constructor argument below.
struct PanelListViewInsets {
    short left = 7;
    short right = 5;
    short icon_gap = 7;
};

class PanelListViewGeometry
{
public:
    using Insets = PanelListViewInsets;

    PanelListViewGeometry();
    PanelListViewGeometry(NSFont *_font, int _icon_scale, unsigned _padding, Insets _insets = {});

    [[nodiscard]] short LineHeight() const { return m_LineHeight; }
    [[nodiscard]] short TextBaseLine() const { return m_TextBaseLine; }
    [[nodiscard]] short IconSize() const { return m_IconSize; }
    [[nodiscard]] short LeftInset() const noexcept { return m_Insets.left; }
    [[nodiscard]] short RightInset() const noexcept { return m_Insets.right; }
    [[nodiscard]] short IconGap() const noexcept { return m_Insets.icon_gap; }
    [[nodiscard]] static short TopInset() { return 1; }
    [[nodiscard]] static short BottomInset() { return 1; }

    // Returns the the left offset of the filename text in its column
    [[nodiscard]] short FilenameOffsetInColumn() const noexcept;

private:
    Insets m_Insets;
    short m_LineHeight;
    short m_TextBaseLine;
    short m_IconSize;
};

} // namespace nc::panel
