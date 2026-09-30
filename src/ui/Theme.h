#pragma once
#include "application/Settings.h"

#include <d2d1.h>

namespace da::ui {

struct Palette {
    bool dark = false;
    D2D1_COLOR_F bg, card, cardBorder, divider;
    D2D1_COLOR_F text, text2, text3;
    D2D1_COLOR_F accent, accentHover, accentPressed, onAccent;
    D2D1_COLOR_F control, controlHover, controlPressed, controlBorder;
    D2D1_COLOR_F track, thumb, toggleOff;
    D2D1_COLOR_F success, warning, error;
    D2D1_COLOR_F infoBg, warnBg, errorBg, successBg;
    D2D1_COLOR_F focus;
    D2D1_COLOR_F popup, popupBorder, selection;
};

D2D1_COLOR_F Rgb(unsigned rgb, float a = 1.0f);
const Palette& LightPalette();
const Palette& DarkPalette();
// Windows "app mode" (Settings > Personalization > Colors). Read-only query.
bool SystemPrefersDark();
bool ResolveDark(ThemeMode mode);

} // namespace da::ui
