#include "ui/MainWindow.h"

#include "diagnostics/Logger.h"
#include "resources/resource.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace da::ui {

namespace {
constexpr UINT_PTR kTelemetryTimer = 1;
constexpr float kWindowW = 480, kWindowH = 780;
constexpr float kMinW = 400, kMinH = 520;
constexpr float kMaxContentW = 560;

// Documented DWM attributes (dwmapi.h, Windows 11). Defined here for older SDK headers.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmCaptionColor = 35;
} // namespace

bool MainWindow::Create(HINSTANCE instance, ApplicationController* controller, bool startMinimized) {
    ctl_ = controller;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    // Size for the primary monitor's DPI; WM_DPICHANGED handles moves to other monitors.
    const UINT dpi = GetDpiForSystem();
    RECT rc{0, 0, static_cast<LONG>(kWindowW * dpi / 96), static_cast<LONG>(kWindowH * dpi / 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    hwnd_ = CreateWindowExW(0, kClassName, L"DualAudio", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, startMinimized ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void MainWindow::ApplyTheme() {
    dark_ = ResolveDark(ui_.theme);
    const BOOL darkBar = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &darkBar, sizeof(darkBar));
    const D2D1_COLOR_F bg = dark_ ? DarkPalette().bg : LightPalette().bg;
    const COLORREF caption = RGB(static_cast<int>(bg.r * 255), static_cast<int>(bg.g * 255), static_cast<int>(bg.b * 255));
    DwmSetWindowAttribute(hwnd_, kDwmCaptionColor, &caption, sizeof(caption));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::PullState() {
    const ThemeMode oldTheme = ui_.theme;
    state_ = ctl_->State();
    // Keep the value the user is dragging; take everything else from the controller.
    const Settings dragged = ui_;
    ui_ = state_.settings;
    if (gui_.Capturing()) {
        for (int i = 0; i < 2; ++i) {
            ui_.volume[i] = dragged.volume[i];
            ui_.offsetMs[i] = dragged.offsetMs[i];
        }
    }
    if (ui_.theme != oldTheme) ApplyTheme();
}

void MainWindow::ResetTransientInput() {
    input_.pressed = input_.released = false;
    input_.wheel = 0;
    input_.tab = input_.enter = input_.space = input_.escape = false;
    input_.left = input_.right = input_.up = input_.down_ = false;
}

void MainWindow::Navigate(Page p) {
    page_ = p;
    scroll_[static_cast<int>(p)] = 0;
    if (p == Page::Settings && ui_.source == SourceMode::SingleApp) ctl_->RefreshApps();
}

void MainWindow::Frame() {
    if (rendering_) return;
    rendering_ = true;
    for (int pass = 0; pass < 3; ++pass) {
        tel_ = ctl_->Telemetry();
        const Palette& pal = dark_ ? DarkPalette() : LightPalette();
        if (!canvas_.Begin(pal.bg)) break;
        const int pi = static_cast<int>(page_);
        const float viewH = canvas_.Height();
        scroll_[pi] = std::clamp(scroll_[pi], 0.0f, std::max(0.0f, contentHeight_ - viewH));
        // Content is a centered column so wide/maximized windows stay readable.
        const float fullW = canvas_.Width();
        const float w = std::min(fullW, kMaxContentW);
        const float ox = std::floor((fullW - w) / 2);
        gui_.Begin(&canvas_, &pal, input_, scroll_[pi], ox);
        canvas_.SetTranslation(ox, -scroll_[pi]);
        switch (page_) {
        case Page::Main: contentHeight_ = DrawMain(w); break;
        case Page::Settings: contentHeight_ = DrawSettings(w); break;
        case Page::Diagnostics: contentHeight_ = DrawDiagnostics(w); break;
        }
        gui_.End();
        canvas_.SetTranslation(0, 0);
        if (contentHeight_ > viewH + 1) {
            // Thin overlay scroll indicator.
            const float track = viewH - 8;
            const float thumb = std::max(40.0f, track * viewH / contentHeight_);
            const float t = scroll_[pi] / (contentHeight_ - viewH);
            canvas_.Fill(R(fullW - 6, 4 + t * (track - thumb), 3, thumb), pal.text3, 1.5f);
        }
        canvas_.End();
        const bool escapeToMain = input_.escape && !gui_.ConsumedEscape() && page_ != Page::Main;
        ResetTransientInput();
        if (escapeToMain) {
            Navigate(Page::Main);
            continue;
        }
        if (!gui_.NeedsAnotherFrame()) break;
    }
    rendering_ = false;
}

void MainWindow::OnMouse(LPARAM lp) {
    const float scale = 96.0f / canvas_.Dpi();
    input_.mx = GET_X_LPARAM(lp) * scale;
    input_.my = GET_Y_LPARAM(lp) * scale;
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        if (!canvas_.Init(hwnd_)) return -1;
        ui_ = ctl_->CurrentSettings();
        ApplyTheme();
        SetTimer(hwnd_, kTelemetryTimer, 250, nullptr);
        return 0;
    case WM_SIZE:
        canvas_.Resize();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        canvas_.SetDpi(static_cast<float>(HIWORD(wp)));
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = GetDpiForWindow(hwnd_) / 96.0f;
        mm->ptMinTrackSize.x = static_cast<LONG>(kMinW * s);
        mm->ptMinTrackSize.y = static_cast<LONG>(kMinH * s);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Frame();
        return 0;
    }
    case WM_MOUSEMOVE:
        OnMouse(lp);
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        Frame();
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!input_.down) {
            input_.mx = input_.my = -1;
            Frame();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        OnMouse(lp);
        SetCapture(hwnd_);
        input_.down = true;
        input_.pressed = true;
        Frame();
        return 0;
    case WM_LBUTTONUP:
        OnMouse(lp);
        ReleaseCapture();
        input_.down = false;
        input_.released = true;
        Frame();
        return 0;
    case WM_CAPTURECHANGED:
        if (input_.down) {
            input_.down = false;
            input_.released = true;
            Frame();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (!gui_.PopupOpen()) {
            scroll_[static_cast<int>(page_)] -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 60.0f;
            Frame();
        }
        return 0;
    case WM_KEYDOWN: {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        input_.shift = shift;
        switch (wp) {
        case VK_TAB: input_.tab = true; break;
        case VK_RETURN: input_.enter = true; break;
        case VK_SPACE: input_.space = true; break;
        case VK_ESCAPE: input_.escape = true; break;
        case VK_LEFT: input_.left = true; break;
        case VK_RIGHT: input_.right = true; break;
        case VK_UP: input_.up = true; break;
        case VK_DOWN: input_.down_ = true; break;
        case VK_PRIOR: scroll_[static_cast<int>(page_)] -= canvas_.Height() * 0.8f; break;
        case VK_NEXT: scroll_[static_cast<int>(page_)] += canvas_.Height() * 0.8f; break;
        default: return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        Frame();
        return 0;
    }
    case WM_TIMER:
        if (wp == kTelemetryTimer && !IsIconic(hwnd_)) {
            if (state_.running || state_.calibrating || page_ == Page::Diagnostics) Frame();
        }
        return 0;
    case kStateMessage:
        PullState();
        Frame();
        return 0;
    case kRedrawMessage:
        Frame();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && ui_.theme == ThemeMode::System)
            ApplyTheme();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kTelemetryTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace da::ui
