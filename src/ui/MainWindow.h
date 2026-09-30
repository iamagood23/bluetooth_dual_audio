#pragma once
#include "application/ApplicationController.h"
#include "ui/Canvas.h"
#include "ui/Gui.h"

#include <windows.h>

namespace da::ui {

class MainWindow {
public:
    static constexpr UINT kStateMessage = WM_APP + 1;
    static constexpr UINT kRedrawMessage = WM_APP + 2;
    static constexpr const wchar_t* kClassName = L"DualAudio.MainWindow";

    bool Create(HINSTANCE instance, ApplicationController* controller, bool startMinimized);
    HWND Hwnd() const { return hwnd_; }

private:
    enum class Page { Main = 0, Settings, Diagnostics };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Frame();
    void ApplyTheme();
    void PullState();
    void OnMouse(LPARAM lp);
    void ResetTransientInput();
    void Navigate(Page p);

    float DrawMain(float width);
    float DrawSettings(float width);
    float DrawDiagnostics(float width);
    float DrawOutputCard(int slot, float x, float y, float w);
    float DrawSyncCard(float x, float y, float w);
    float DrawBanner(float x, float y, float w);
    float Header(const std::wstring& title, float width, bool back);
    float SectionTitle(const std::wstring& t, float x, float y, float w);
    float ToggleRow(int id, const std::wstring& label, const std::wstring& hint, bool& value, float x, float y, float w,
                    bool enabled = true);
    float KeyValue(const std::wstring& k, const std::wstring& v, float x, float y, float w,
                   const D2D1_COLOR_F* valueColor = nullptr);

    HWND hwnd_ = nullptr;
    ApplicationController* ctl_ = nullptr;
    Canvas canvas_;
    Gui gui_;
    InputState input_;
    Page page_ = Page::Main;
    float scroll_[3] = {0, 0, 0};
    float contentHeight_ = 0;
    bool dark_ = false;
    bool rendering_ = false;
    bool tracking_ = false;
    bool showFineTune_ = false;

    AppState state_;
    EngineTelemetry tel_;
    Settings ui_; // local mirror of settings for immediate feedback
};

} // namespace da::ui
