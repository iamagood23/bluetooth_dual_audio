#pragma once
// Small immediate-mode widget toolkit drawn with Direct2D, styled after Windows 11 (Fluent)
// controls. Every frame the page code lays out and draws widgets; interaction results are
// returned directly. Supports mouse and keyboard (Tab / Shift+Tab, Enter/Space, arrows, Esc).
#include "ui/Canvas.h"
#include "ui/Theme.h"

#include <string>
#include <vector>

namespace da::ui {

struct InputState {
    float mx = -1, my = -1; // window DIPs; negative = outside
    bool down = false;
    bool pressed = false;
    bool released = false;
    float wheel = 0;
    bool tab = false, shift = false, enter = false, space = false, escape = false;
    bool left = false, right = false, up = false, down_ = false;
};

struct DropItem {
    std::wstring label;
    std::wstring detail;
    wchar_t icon = 0;
};

enum class ButtonKind { Primary, Secondary, Subtle };

inline D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

class Gui {
public:
    void Begin(Canvas* canvas, const Palette* palette, const InputState& in, float scrollY, float offsetX = 0);
    void End();
    bool NeedsAnotherFrame() const { return again_; }
    bool Capturing() const { return active_ != 0; }
    bool PopupOpen() const { return popupId_ != 0 && !popupClosing_; }
    bool ConsumedEscape() const { return escapeUsed_; }

    const Palette& P() const { return *p_; }
    Canvas& C() { return *c_; }

    void Card(const D2D1_RECT_F& r);
    bool Button(int id, const D2D1_RECT_F& r, const std::wstring& label, ButtonKind kind = ButtonKind::Secondary,
                bool enabled = true, wchar_t icon = 0);
    bool IconButton(int id, const D2D1_RECT_F& r, wchar_t glyph, bool enabled = true, const D2D1_COLOR_F* tint = nullptr);
    bool Link(int id, const D2D1_RECT_F& r, const std::wstring& label, wchar_t icon = 0, Align align = Align::Left);
    bool Toggle(int id, const D2D1_RECT_F& r, bool& value, bool enabled = true);
    bool Slider(int id, const D2D1_RECT_F& r, float& value, float lo, float hi, float keyStep, bool enabled = true);
    bool Segmented(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int& index, bool enabled = true);
    bool Dropdown(int id, const D2D1_RECT_F& r, const std::vector<DropItem>& items, int& index,
                  const std::wstring& placeholder, bool enabled = true);
    bool Disclosure(int id, const D2D1_RECT_F& r, const std::wstring& label, bool& open);

private:
    bool Hover(const D2D1_RECT_F& r, int id) const;
    void Focusable(int id);
    bool Focused(int id) const { return focus_ == id; }
    bool Activated(int id) const; // Enter/Space on the focused widget
    void FocusRing(const D2D1_RECT_F& r, float radius);
    void DrawPopup();

    Canvas* c_ = nullptr;
    const Palette* p_ = nullptr;
    InputState in_;
    float mx_ = -1, my_ = -1; // content coordinates
    float scroll_ = 0;
    int active_ = 0;
    int focus_ = 0;
    bool focusVisible_ = false;
    bool again_ = false;
    bool escapeUsed_ = false;
    std::vector<int> order_, prevOrder_;
    int tabMove_ = 0;

    // Dropdown popup.
    int popupId_ = 0;
    bool popupClosing_ = false;
    D2D1_RECT_F popupAnchor_{};
    std::vector<DropItem> popupItems_;
    int popupHover_ = -1;
    int popupCurrent_ = -1;
    int popupResult_ = -1;
    int popupResultFor_ = 0;
    bool popupFromKeyboard_ = false;
};

} // namespace da::ui
