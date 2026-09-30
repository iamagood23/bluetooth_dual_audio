#pragma once
// Thin Direct2D + DirectWrite drawing layer. All coordinates are device-independent pixels.
#include "common/ComUtil.h"

#include <d2d1.h>
#include <dwrite.h>

#include <map>
#include <string>

namespace da::ui {

enum class Align { Left, Center, Right };

class Canvas {
public:
    bool Init(HWND hwnd);
    void Resize();
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }
    float Width() const { return width_; }
    float Height() const { return height_; }

    bool Begin(const D2D1_COLOR_F& clear);
    void End(); // recreates the target on device loss

    void SetTranslation(float dx, float dy);
    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0, float width = 1);
    void FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c);
    void StrokeCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c, float width = 1);
    void Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width = 1);

    // Single-line text (ellipsized) or wrapped text; vertically centered for single line.
    void Text(const std::wstring& s, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float size,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, Align align = Align::Left, bool wrap = false);
    float TextHeight(const std::wstring& s, float width, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    void Icon(wchar_t glyph, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float size);

private:
    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, bool icon);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
                                     bool icon, bool wrap, Align align);
    void CreateTarget();
    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& c);

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring textFamily_ = L"Segoe UI";
    std::wstring iconFamily_ = L"Segoe MDL2 Assets";
    float dpi_ = 96.0f;
    float width_ = 0, height_ = 0;
};

} // namespace da::ui
