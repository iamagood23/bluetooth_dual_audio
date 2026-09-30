#include "ui/Canvas.h"

namespace da::ui {

namespace {
bool HasFamily(IDWriteFactory* f, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(f->GetSystemFontCollection(&fonts))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(name, &idx, &exists)) && exists;
}
} // namespace

bool Canvas::Init(HWND hwnd) {
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    // Windows 11 fonts, with Windows 10 fallbacks.
    if (HasFamily(dwrite_.Get(), L"Segoe UI Variable Text")) textFamily_ = L"Segoe UI Variable Text";
    if (HasFamily(dwrite_.Get(), L"Segoe Fluent Icons")) iconFamily_ = L"Segoe Fluent Icons";
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    CreateTarget();
    return target_ != nullptr;
}

void Canvas::CreateTarget() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left), static_cast<UINT32>(rc.bottom - rc.top));
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), dpi_, dpi_);
    target_.Reset();
    brush_.Reset();
    factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, size), &target_);
    if (target_) {
        target_->SetDpi(dpi_, dpi_);
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    }
    width_ = size.width * 96.0f / dpi_;
    height_ = size.height * 96.0f / dpi_;
}

void Canvas::Resize() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (target_) target_->Resize(D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    width_ = rc.right * 96.0f / dpi_;
    height_ = rc.bottom * 96.0f / dpi_;
}

void Canvas::SetDpi(float dpi) {
    dpi_ = dpi;
    if (target_) target_->SetDpi(dpi, dpi);
    Resize();
}

bool Canvas::Begin(const D2D1_COLOR_F& clear) {
    if (!target_) CreateTarget();
    if (!target_) return false;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(clear);
    return true;
}

void Canvas::End() {
    if (!target_) return;
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        brush_.Reset();
    }
}

void Canvas::SetTranslation(float dx, float dy) { target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy)); }
void Canvas::PushClip(const D2D1_RECT_F& r) { target_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::PopClip() { target_->PopAxisAlignedClip(); }

ID2D1SolidColorBrush* Canvas::Brush(const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    return brush_.Get();
}

void Canvas::Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
    if (radius > 0)
        target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    else
        target_->FillRectangle(r, Brush(c));
}

void Canvas::Stroke(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius, float width) {
    const float h = width / 2;
    const D2D1_RECT_F in = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
    if (radius > 0)
        target_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), Brush(c), width);
    else
        target_->DrawRectangle(in, Brush(c), width);
}

void Canvas::FillCircle(float cx, float cy, float r, const D2D1_COLOR_F& c) {
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(c));
}

void Canvas::StrokeCircle(float cx, float cy, float r, const D2D1_COLOR_F& c, float width) {
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), Brush(c), width);
}

void Canvas::Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width) {
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), Brush(c), width);
}

IDWriteTextFormat* Canvas::Format(float size, DWRITE_FONT_WEIGHT weight, bool icon) {
    const uint64_t key = (static_cast<uint64_t>(size * 100) << 20) | (static_cast<uint64_t>(weight) << 1) | (icon ? 1 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    dwrite_->CreateTextFormat(icon ? iconFamily_.c_str() : textFamily_.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    formats_[key] = f;
    return f.Get();
}

ComPtr<IDWriteTextLayout> Canvas::Layout(const std::wstring& s, float w, float h, float size, DWRITE_FONT_WEIGHT weight,
                                         bool icon, bool wrap, Align align) {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* fmt = Format(size, weight, icon);
    if (!fmt || FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, std::max(w, 1.0f),
                                                 std::max(h, 1.0f), &layout)))
        return nullptr;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align == Align::Center  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite_->CreateEllipsisTrimmingSign(fmt, &ellipsis);
        layout->SetTrimming(&trim, ellipsis.Get());
    }
    return layout;
}

void Canvas::Text(const std::wstring& s, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float size,
                  DWRITE_FONT_WEIGHT weight, Align align, bool wrap) {
    if (s.empty()) return;
    auto layout = Layout(s, r.right - r.left, r.bottom - r.top, size, weight, false, wrap, align);
    if (layout) target_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Canvas::TextHeight(const std::wstring& s, float width, float size, DWRITE_FONT_WEIGHT weight) {
    auto layout = Layout(s, width, 10000.0f, size, weight, false, true, Align::Left);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return size * 1.4f;
    return m.height;
}

float Canvas::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight) {
    auto layout = Layout(s, 10000.0f, size * 2, size, weight, false, false, Align::Left);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

void Canvas::Icon(wchar_t glyph, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float size) {
    const std::wstring s(1, glyph);
    auto layout = Layout(s, r.right - r.left, r.bottom - r.top, size, DWRITE_FONT_WEIGHT_NORMAL, true, false, Align::Center);
    if (layout) target_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), Brush(c));
}

} // namespace da::ui
