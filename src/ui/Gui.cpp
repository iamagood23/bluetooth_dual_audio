#include "ui/Gui.h"

#include <algorithm>
#include <cmath>

namespace da::ui {

namespace {
constexpr wchar_t kChevronDown = 0xE70D;
constexpr wchar_t kChevronUp = 0xE70E;
constexpr float kItemH = 44.0f;

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
} // namespace

void Gui::Begin(Canvas* canvas, const Palette* palette, const InputState& in, float scrollY, float offsetX) {
    c_ = canvas;
    p_ = palette;
    in_ = in;
    scroll_ = scrollY;
    mx_ = in.mx < 0 ? -1 : in.mx - offsetX;
    my_ = in.my < 0 ? -1 : in.my + scrollY;
    again_ = false;
    escapeUsed_ = false;
    prevOrder_.swap(order_);
    order_.clear();
    tabMove_ = 0;
    if (in.tab && !PopupOpen()) tabMove_ = in.shift ? -1 : 1;
    if (tabMove_ != 0 && !prevOrder_.empty()) {
        auto it = std::find(prevOrder_.begin(), prevOrder_.end(), focus_);
        int idx = it == prevOrder_.end() ? (tabMove_ > 0 ? -1 : 0) : static_cast<int>(it - prevOrder_.begin());
        idx = (idx + tabMove_ + static_cast<int>(prevOrder_.size())) % static_cast<int>(prevOrder_.size());
        focus_ = prevOrder_[static_cast<size_t>(idx)];
        focusVisible_ = true;
        again_ = true;
    }
    if (in.pressed) focusVisible_ = false;
    if (popupClosing_) {
        popupId_ = 0;
        popupClosing_ = false;
    }
}

void Gui::End() {
    if (popupId_) DrawPopup();
    if (in_.released) active_ = 0;
    // Focus that vanished (widget no longer drawn) is dropped.
    if (focus_ && std::find(order_.begin(), order_.end(), focus_) == order_.end()) focus_ = 0;
}

bool Gui::Hover(const D2D1_RECT_F& r, int id) const {
    if (popupId_) return false;             // an open popup owns the mouse
    if (active_ && active_ != id) return false; // another widget is being pressed/dragged
    return mx_ >= r.left && mx_ < r.right && my_ >= r.top && my_ < r.bottom;
}

void Gui::Focusable(int id) { order_.push_back(id); }

bool Gui::Activated(int id) const { return focus_ == id && !popupId_ && (in_.enter || in_.space); }

void Gui::FocusRing(const D2D1_RECT_F& r, float radius) {
    if (!focusVisible_) return;
    c_->Stroke(Inflate(r, 3), p_->focus, radius + 3, 2.0f);
}

void Gui::Card(const D2D1_RECT_F& r) {
    c_->Fill(r, p_->card, 8);
    c_->Stroke(r, p_->cardBorder, 8, 1);
}

bool Gui::Button(int id, const D2D1_RECT_F& r, const std::wstring& label, ButtonKind kind, bool enabled, wchar_t icon) {
    Focusable(id);
    const bool hover = enabled && Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool pressedLook = active_ == id && hover && in_.down;
    bool clicked = enabled && ((active_ == id && in_.released && hover) || Activated(id));

    D2D1_COLOR_F fill, text;
    const float radius = 6;
    switch (kind) {
    case ButtonKind::Primary:
        fill = !enabled ? WithAlpha(p_->text3, 0.35f) : pressedLook ? p_->accentPressed : hover ? p_->accentHover : p_->accent;
        text = enabled ? p_->onAccent : p_->text3;
        c_->Fill(r, fill, radius);
        break;
    case ButtonKind::Secondary:
        fill = pressedLook ? p_->controlPressed : hover ? p_->controlHover : p_->control;
        text = enabled ? p_->text : p_->text3;
        c_->Fill(r, fill, radius);
        c_->Stroke(r, p_->controlBorder, radius, 1);
        break;
    case ButtonKind::Subtle:
        if (hover) c_->Fill(r, pressedLook ? p_->controlPressed : p_->controlHover, radius);
        text = enabled ? p_->text : p_->text3;
        break;
    }
    const float h = r.bottom - r.top;
    const float fontSize = h >= 40 ? 15.0f : 14.0f;
    if (icon) {
        const float tw = c_->TextWidth(label, fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float total = 16 + 8 + tw;
        const float x = (r.left + r.right - total) / 2;
        c_->Icon(icon, R(x, r.top, 16, h), text, 15);
        c_->Text(label, R(x + 24, r.top, tw + 4, h), text, fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        c_->Text(label, r, text, fontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
    }
    FocusRing(r, radius);
    return clicked;
}

bool Gui::IconButton(int id, const D2D1_RECT_F& r, wchar_t glyph, bool enabled, const D2D1_COLOR_F* tint) {
    Focusable(id);
    const bool hover = enabled && Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool clicked = enabled && ((active_ == id && in_.released && hover) || Activated(id));
    if (hover) c_->Fill(r, active_ == id && in_.down ? p_->controlPressed : p_->controlHover, 6);
    c_->Icon(glyph, r, tint ? *tint : (enabled ? p_->text : p_->text3), 16);
    FocusRing(r, 6);
    return clicked;
}

bool Gui::Link(int id, const D2D1_RECT_F& r, const std::wstring& label, wchar_t icon, Align align) {
    Focusable(id);
    const bool hover = Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool clicked = (active_ == id && in_.released && hover) || Activated(id);
    const D2D1_COLOR_F col = hover ? p_->accentHover : p_->accent;
    float x = r.left;
    const float tw = c_->TextWidth(label, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float total = tw + (icon ? 22.0f : 0.0f);
    if (align == Align::Center) x = (r.left + r.right - total) / 2;
    if (align == Align::Right) x = r.right - total;
    if (icon) {
        c_->Icon(icon, R(x, r.top, 16, r.bottom - r.top), col, 14);
        x += 22;
    }
    c_->Text(label, R(x, r.top, tw + 4, r.bottom - r.top), col, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (hover) c_->Line(x, r.bottom - 5, x + tw, r.bottom - 5, col, 1);
    FocusRing(R(x - (icon ? 22.0f : 0.0f), r.top + 2, total, r.bottom - r.top - 4), 4);
    return clicked;
}

bool Gui::Toggle(int id, const D2D1_RECT_F& r, bool& value, bool enabled) {
    Focusable(id);
    const bool hover = enabled && Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool clicked = enabled && ((active_ == id && in_.released && hover) || Activated(id));
    if (clicked) value = !value;
    const float w = 40, h = 20;
    const D2D1_RECT_F sw = R(r.right - w, (r.top + r.bottom - h) / 2, w, h);
    const float cy = (sw.top + sw.bottom) / 2;
    const float knob = hover ? 7.0f : 6.0f;
    if (value) {
        c_->Fill(sw, enabled ? (hover ? p_->accentHover : p_->accent) : WithAlpha(p_->text3, 0.5f), h / 2);
        c_->FillCircle(sw.right - 10, cy, knob, p_->onAccent);
    } else {
        if (hover) c_->Fill(sw, p_->controlHover, h / 2);
        c_->Stroke(sw, enabled ? p_->toggleOff : WithAlpha(p_->text3, 0.5f), h / 2, 1);
        c_->FillCircle(sw.left + 10, cy, knob, enabled ? p_->toggleOff : WithAlpha(p_->text3, 0.5f));
    }
    FocusRing(sw, h / 2);
    return clicked;
}

bool Gui::Slider(int id, const D2D1_RECT_F& r, float& value, float lo, float hi, float keyStep, bool enabled) {
    Focusable(id);
    const bool hover = enabled && Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const float x0 = r.left + 10, x1 = r.right - 10;
    const float cy = (r.top + r.bottom) / 2;
    bool changed = false;
    if (enabled && active_ == id && in_.down) {
        float t = std::clamp((mx_ - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        const float v = lo + t * (hi - lo);
        if (v != value) {
            value = v;
            changed = true;
        }
    }
    if (enabled && Focused(id) && !popupId_ && (in_.left || in_.right || in_.up || in_.down_)) {
        const float d = (in_.right || in_.up) ? keyStep : -keyStep;
        const float v = std::clamp(value + d, lo, hi);
        if (v != value) {
            value = v;
            changed = true;
        }
        focusVisible_ = true;
    }
    const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
    const float tx = x0 + t * (x1 - x0);
    c_->Fill(D2D1::RectF(x0, cy - 2, x1, cy + 2), p_->track, 2);
    c_->Fill(D2D1::RectF(x0, cy - 2, tx, cy + 2), enabled ? p_->accent : p_->text3, 2);
    c_->FillCircle(tx, cy, 10, p_->thumb);
    c_->StrokeCircle(tx, cy, 10, p_->controlBorder, 1);
    const float inner = (active_ == id) ? 5.0f : hover ? 7.0f : 6.0f;
    c_->FillCircle(tx, cy, inner, enabled ? p_->accent : p_->text3);
    FocusRing(r, 6);
    return changed;
}

bool Gui::Segmented(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int& index, bool enabled) {
    Focusable(id);
    const int n = static_cast<int>(options.size());
    if (n == 0) return false;
    const float w = (r.right - r.left) / n;
    bool changed = false;
    c_->Fill(r, p_->control, 6);
    c_->Stroke(r, p_->controlBorder, 6, 1);
    for (int i = 0; i < n; ++i) {
        const D2D1_RECT_F s = R(r.left + i * w, r.top, w, r.bottom - r.top);
        const bool hover = enabled && Hover(s, id);
        if (hover && in_.pressed) {
            active_ = id;
            focus_ = id;
        }
        if (enabled && hover && in_.released && active_ == id && index != i) {
            index = i;
            changed = true;
        }
        const bool sel = i == index;
        if (sel)
            c_->Fill(Inflate(s, -3), enabled ? p_->accent : WithAlpha(p_->text3, 0.4f), 4);
        else if (hover)
            c_->Fill(Inflate(s, -3), p_->controlHover, 4);
        c_->Text(options[static_cast<size_t>(i)], s, sel ? p_->onAccent : (enabled ? p_->text : p_->text3), 13,
                 sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    }
    if (enabled && Focused(id) && !popupId_ && (in_.left || in_.right)) {
        const int ni = std::clamp(index + (in_.right ? 1 : -1), 0, n - 1);
        if (ni != index) {
            index = ni;
            changed = true;
        }
        focusVisible_ = true;
    }
    FocusRing(r, 6);
    return changed;
}

bool Gui::Dropdown(int id, const D2D1_RECT_F& r, const std::vector<DropItem>& items, int& index,
                   const std::wstring& placeholder, bool enabled) {
    Focusable(id);
    bool changed = false;
    if (popupResultFor_ == id && popupResult_ >= 0) {
        if (popupResult_ != index) {
            index = popupResult_;
            changed = true;
        }
        popupResult_ = -1;
        popupResultFor_ = 0;
    }
    const bool hover = enabled && Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool open = popupId_ == id;
    if (enabled && !open && ((active_ == id && in_.released && hover) || Activated(id))) {
        popupId_ = id;
        popupClosing_ = false;
        popupAnchor_ = r;
        popupItems_ = items;
        popupCurrent_ = index;
        popupHover_ = index >= 0 ? index : 0;
        popupFromKeyboard_ = Activated(id);
        again_ = true;
    }

    c_->Fill(r, open ? p_->controlPressed : hover ? p_->controlHover : p_->control, 6);
    c_->Stroke(r, open ? p_->accent : p_->controlBorder, 6, 1);
    const float h = r.bottom - r.top;
    float x = r.left + 12;
    const D2D1_COLOR_F tc = enabled ? p_->text : p_->text3;
    if (index >= 0 && index < static_cast<int>(items.size())) {
        const DropItem& it = items[static_cast<size_t>(index)];
        if (it.icon) {
            c_->Icon(it.icon, R(x, r.top, 18, h), tc, 16);
            x += 28;
        }
        const float dw = it.detail.empty() ? 0.0f : std::min(c_->TextWidth(it.detail, 12) + 12, 120.0f);
        c_->Text(it.label, D2D1::RectF(x, r.top, r.right - 40 - dw, r.bottom), tc, 14);
        if (dw > 0) c_->Text(it.detail, D2D1::RectF(r.right - 36 - dw, r.top, r.right - 36, r.bottom), p_->text3, 12,
                             DWRITE_FONT_WEIGHT_NORMAL, Align::Right);
    } else {
        c_->Text(placeholder, D2D1::RectF(x, r.top, r.right - 40, r.bottom), p_->text3, 14);
    }
    c_->Icon(open ? kChevronUp : kChevronDown, R(r.right - 34, r.top, 24, h), p_->text2, 11);
    FocusRing(r, 6);
    return changed;
}

bool Gui::Disclosure(int id, const D2D1_RECT_F& r, const std::wstring& label, bool& open) {
    Focusable(id);
    const bool hover = Hover(r, id);
    if (hover && in_.pressed) {
        active_ = id;
        focus_ = id;
    }
    const bool clicked = (active_ == id && in_.released && hover) || Activated(id);
    if (clicked) open = !open;
    if (hover) c_->Fill(r, p_->controlHover, 6);
    c_->Text(label, D2D1::RectF(r.left + 8, r.top, r.right - 36, r.bottom), p_->text, 14);
    c_->Icon(open ? kChevronUp : kChevronDown, R(r.right - 32, r.top, 24, r.bottom - r.top), p_->text2, 11);
    FocusRing(r, 6);
    return clicked;
}

void Gui::DrawPopup() {
    const int n = static_cast<int>(popupItems_.size());
    const float w = popupAnchor_.right - popupAnchor_.left;
    const float listH = std::max(1, n) * kItemH + 8;
    // Prefer below the control; flip above if it would leave the visible area.
    const float viewTop = scroll_, viewBottom = scroll_ + c_->Height();
    float top = popupAnchor_.bottom + 4;
    if (top + listH > viewBottom - 8 && popupAnchor_.top - 4 - listH >= viewTop + 8) top = popupAnchor_.top - 4 - listH;
    const D2D1_RECT_F box = R(popupAnchor_.left, top, w, std::min(listH, std::max(viewBottom - top - 8, kItemH + 8)));

    // Input.
    const bool inside = mx_ >= box.left && mx_ < box.right && my_ >= box.top && my_ < box.bottom;
    int hoverItem = -1;
    if (inside) hoverItem = static_cast<int>((my_ - box.top - 4) / kItemH);
    if (hoverItem >= n) hoverItem = -1;
    if (hoverItem >= 0 && (in_.mx >= 0)) popupHover_ = hoverItem;
    bool close = false;
    if (in_.up) popupHover_ = std::max(0, popupHover_ - 1);
    if (in_.down_) popupHover_ = std::min(n - 1, popupHover_ + 1);
    if ((in_.enter || in_.space) && !popupFromKeyboard_ && popupHover_ >= 0) {
        popupResult_ = popupHover_;
        popupResultFor_ = popupId_;
        close = true;
    }
    popupFromKeyboard_ = false;
    if (in_.escape) {
        close = true;
        escapeUsed_ = true;
    }
    if (in_.pressed) {
        if (inside && hoverItem >= 0) {
            popupResult_ = hoverItem;
            popupResultFor_ = popupId_;
        }
        close = true;
        active_ = 0;
    }

    // Draw (shadow, surface, items).
    c_->Fill(Inflate(box, 2), WithAlpha(D2D1::ColorF(0, 0, 0), p_->dark ? 0.35f : 0.08f), 10);
    c_->Fill(box, p_->popup, 8);
    c_->Stroke(box, p_->popupBorder, 8, 1);
    c_->PushClip(box);
    for (int i = 0; i < n; ++i) {
        const DropItem& it = popupItems_[static_cast<size_t>(i)];
        const D2D1_RECT_F ir = R(box.left + 4, box.top + 4 + i * kItemH, w - 8, kItemH);
        if (i == popupHover_) c_->Fill(ir, p_->controlHover, 6);
        if (i == popupCurrent_) {
            c_->Fill(ir, p_->selection, 6);
            c_->Fill(R(ir.left, ir.top + 12, 3, kItemH - 24), p_->accent, 1.5f);
        }
        float x = ir.left + 12;
        if (it.icon) {
            c_->Icon(it.icon, R(x, ir.top, 18, kItemH), p_->text, 16);
            x += 28;
        }
        if (it.detail.empty()) {
            c_->Text(it.label, D2D1::RectF(x, ir.top, ir.right - 8, ir.bottom), p_->text, 14);
        } else {
            c_->Text(it.label, D2D1::RectF(x, ir.top + 3, ir.right - 8, ir.top + 25), p_->text, 14);
            c_->Text(it.detail, D2D1::RectF(x, ir.top + 23, ir.right - 8, ir.bottom - 3), p_->text3, 12);
        }
    }
    c_->PopClip();
    if (close) {
        popupClosing_ = true;
        again_ = true;
    }
}

} // namespace da::ui
