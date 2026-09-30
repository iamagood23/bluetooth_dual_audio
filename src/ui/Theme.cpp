#include "ui/Theme.h"

#include <windows.h>

namespace da::ui {

D2D1_COLOR_F Rgb(unsigned rgb, float a) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

const Palette& LightPalette() {
    static const Palette p = [] {
        Palette x;
        x.dark = false;
        x.bg = Rgb(0xF3F3F3);
        x.card = Rgb(0xFFFFFF);
        x.cardBorder = Rgb(0xE3E3E3);
        x.divider = Rgb(0xEAEAEA);
        x.text = Rgb(0x1B1B1B);
        x.text2 = Rgb(0x5D5D5D);
        x.text3 = Rgb(0x8A8A8A);
        x.accent = Rgb(0x005FB8);
        x.accentHover = Rgb(0x1A6FC0);
        x.accentPressed = Rgb(0x3380C8);
        x.onAccent = Rgb(0xFFFFFF);
        x.control = Rgb(0xFFFFFF);
        x.controlHover = Rgb(0xF5F5F5);
        x.controlPressed = Rgb(0xEBEBEB);
        x.controlBorder = Rgb(0xD0D0D0);
        x.track = Rgb(0xC6C6C6);
        x.thumb = Rgb(0xFFFFFF);
        x.toggleOff = Rgb(0x8A8A8A);
        x.success = Rgb(0x0F7B0F);
        x.warning = Rgb(0x9D5D00);
        x.error = Rgb(0xC42B1C);
        x.infoBg = Rgb(0xEAF3FC);
        x.warnBg = Rgb(0xFFF4CE);
        x.errorBg = Rgb(0xFDE7E9);
        x.successBg = Rgb(0xDFF6DD);
        x.focus = Rgb(0x1B1B1B);
        x.popup = Rgb(0xFBFBFB);
        x.popupBorder = Rgb(0xD5D5D5);
        x.selection = Rgb(0xEBF3FB);
        return x;
    }();
    return p;
}

const Palette& DarkPalette() {
    static const Palette p = [] {
        Palette x;
        x.dark = true;
        x.bg = Rgb(0x1F1F1F);
        x.card = Rgb(0x2B2B2B);
        x.cardBorder = Rgb(0x363636);
        x.divider = Rgb(0x3A3A3A);
        x.text = Rgb(0xFFFFFF);
        x.text2 = Rgb(0xC8C8C8);
        x.text3 = Rgb(0x9A9A9A);
        x.accent = Rgb(0x60CDFF);
        x.accentHover = Rgb(0x5AB9E8);
        x.accentPressed = Rgb(0x52A6D0);
        x.onAccent = Rgb(0x000000);
        x.control = Rgb(0x353535);
        x.controlHover = Rgb(0x3C3C3C);
        x.controlPressed = Rgb(0x303030);
        x.controlBorder = Rgb(0x474747);
        x.track = Rgb(0x8C8C8C);
        x.thumb = Rgb(0x454545);
        x.toggleOff = Rgb(0xC8C8C8);
        x.success = Rgb(0x6CCB5F);
        x.warning = Rgb(0xFCE100);
        x.error = Rgb(0xFF99A4);
        x.infoBg = Rgb(0x1E2B38);
        x.warnBg = Rgb(0x433519);
        x.errorBg = Rgb(0x442726);
        x.successBg = Rgb(0x2A3A28);
        x.focus = Rgb(0xFFFFFF);
        x.popup = Rgb(0x2C2C2C);
        x.popupBorder = Rgb(0x454545);
        x.selection = Rgb(0x3A3A3A);
        return x;
    }();
    return p;
}

bool SystemPrefersDark() {
    DWORD value = 1, size = sizeof(value);
    // Read-only query of the user's app theme preference.
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

bool ResolveDark(ThemeMode mode) {
    if (mode == ThemeMode::Dark) return true;
    if (mode == ThemeMode::Light) return false;
    return SystemPrefersDark();
}

} // namespace da::ui
