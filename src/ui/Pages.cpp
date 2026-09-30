// Page layouts for the main window. Everything displayed comes from the controller's AppState
// and the engine's live telemetry; nothing here is simulated.
#include "ui/MainWindow.h"

#include "diagnostics/Logger.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace da::ui {

namespace {

enum Id {
    idSettings = 1, idDiagnostics, idBack,
    idDrop0 = 10, idDrop1, idMute0, idMute1, idVol0, idVol1,
    idSync = 20, idFineTune, idOff0, idOff1, idOffReset, idCalibrate, idCalibCancel, idCalibDismiss,
    idBannerAction, idStart, idBluetooth, idSound,
    idTheme = 50, idStartMin, idAutoStart, idRunAtStartup, idSource, idApp, idAppRefresh, idBuffer, idRate,
    idStrength, idClip, idMicCal, idShowDiag, idVerbose, idExport, idOpenLogs, idDiagExport,
};

constexpr wchar_t kIconSettings = 0xE713, kIconDiag = 0xE9D9, kIconBack = 0xE72B, kIconBluetooth = 0xE702,
                  kIconSpeakers = 0xE7F5, kIconUsb = 0xE88E, kIconTv = 0xE7F4, kIconVolume = 0xE767,
                  kIconMute = 0xE74F, kIconPlay = 0xE768, kIconStop = 0xE71A, kIconMic = 0xE720, kIconInfo = 0xE946,
                  kIconWarning = 0xE7BA, kIconError = 0xE783, kIconClose = 0xE711, kIconRefresh = 0xE72C,
                  kIconFolder = 0xE838, kIconSave = 0xE74E, kIconCheck = 0xE73E, kIconApp = 0xE71D;

constexpr float kMargin = 20;

wchar_t IconFor(const AudioDeviceInfo& d) {
    switch (d.kind) {
    case DeviceKind::Bluetooth: return kIconBluetooth;
    case DeviceKind::Usb: return kIconUsb;
    case DeviceKind::Hdmi: return kIconTv;
    case DeviceKind::Wired: return kIconSpeakers;
    default: return kIconVolume;
    }
}

std::wstring DeviceDetail(const AudioDeviceInfo& d) {
    std::wstring s = DeviceKindLabel(d.kind);
    if (d.handsFree) s += L" · call quality";
    if (d.isDefault) s += L" · Windows default";
    return s;
}

void Open(const wchar_t* target) { ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL); }

D2D1_COLOR_F StateColor(const Palette& p, OutputUiState s) {
    switch (s) {
    case OutputUiState::Playing: return p.success;
    case OutputUiState::Starting:
    case OutputUiState::Direct: return p.accent;
    case OutputUiState::Disconnected:
    case OutputUiState::Reconnecting: return p.warning;
    case OutputUiState::Unavailable:
    case OutputUiState::Failed: return p.error;
    default: return p.text3;
    }
}

std::wstring Ms(double v, int decimals = 0) { return FormatW(decimals ? L"%.1f ms" : L"%.0f ms", v); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------------------------

float MainWindow::Header(const std::wstring& title, float width, bool back) {
    auto& c = canvas_;
    const Palette& p = gui_.P();
    float x = kMargin;
    if (back) {
        if (gui_.IconButton(idBack, R(x - 6, 16, 36, 36), kIconBack)) Navigate(Page::Main);
        x += 36;
    }
    c.Text(title, R(x, 14, width - x - 100, 40), p.text, back ? 20.0f : 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!back) {
        float bx = width - kMargin - 36;
        if (gui_.IconButton(idSettings, R(bx, 16, 36, 36), kIconSettings)) Navigate(Page::Settings);
        if (ui_.showDiagnostics) {
            bx -= 40;
            if (gui_.IconButton(idDiagnostics, R(bx, 16, 36, 36), kIconDiag)) Navigate(Page::Diagnostics);
        }
    }
    return 60;
}

float MainWindow::SectionTitle(const std::wstring& t, float x, float y, float w) {
    canvas_.Text(t, R(x + 2, y, w, 22), gui_.P().text, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    return 28;
}

float MainWindow::ToggleRow(int id, const std::wstring& label, const std::wstring& hint, bool& value, float x, float y,
                            float w, bool enabled) {
    const Palette& p = gui_.P();
    const float h = hint.empty() ? 44.0f : 58.0f;
    canvas_.Text(label, R(x, y + (hint.empty() ? 0 : 9), w - 60, hint.empty() ? h : 22), enabled ? p.text : p.text3, 14);
    if (!hint.empty()) canvas_.Text(hint, R(x, y + 29, w - 60, 20), p.text3, 12);
    gui_.Toggle(id, R(x + w - 48, y, 48, h), value, enabled);
    return h;
}

float MainWindow::KeyValue(const std::wstring& k, const std::wstring& v, float x, float y, float w,
                           const D2D1_COLOR_F* valueColor) {
    const Palette& p = gui_.P();
    canvas_.Text(k, R(x, y, w * 0.48f, 22), p.text2, 13);
    canvas_.Text(v, R(x + w * 0.48f, y, w * 0.52f, 22), valueColor ? *valueColor : p.text, 13, DWRITE_FONT_WEIGHT_NORMAL,
                 Align::Right);
    return 22;
}

// ---------------------------------------------------------------------------------------------
// Main page
// ---------------------------------------------------------------------------------------------

float MainWindow::DrawOutputCard(int slot, float x, float y, float w) {
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const OutputView& v = state_.out[slot];
    const OutputTelemetry& o = tel_.out[slot];
    const float pad = 16, H = 164;
    gui_.Card(R(x, y, w, H));
    const float ix = x + pad, iw = w - 2 * pad;

    // Title row with a live status chip.
    c.Text(slot == 0 ? L"Output 1" : L"Output 2", R(ix, y + 12, 120, 22), p.text2, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring label = OutputUiStateLabel(v.state);
    const float lw = c.TextWidth(label, 12);
    const D2D1_COLOR_F sc = StateColor(p, v.state);
    c.FillCircle(ix + iw - lw - 10, y + 23, 4, sc);
    c.Text(label, R(ix + iw - lw - 2, y + 12, lw + 4, 22), p.text2, 12);

    // Device picker: every active Windows render endpoint, plus "None".
    std::vector<DropItem> items{{L"None", L"", 0}};
    std::vector<std::wstring> ids{L""};
    int index = 0;
    for (const auto& d : state_.devices) {
        items.push_back({d.name, DeviceDetail(d), IconFor(d)});
        ids.push_back(d.id);
        if (d.id == ui_.outputId[slot]) index = static_cast<int>(ids.size()) - 1;
    }
    if (!ui_.outputId[slot].empty() && index == 0) {
        items.push_back({ui_.outputName[slot].empty() ? L"Saved device" : ui_.outputName[slot], L"Not connected",
                         kIconBluetooth});
        ids.push_back(ui_.outputId[slot]);
        index = static_cast<int>(ids.size()) - 1;
    }
    int sel = index;
    if (gui_.Dropdown(slot == 0 ? idDrop0 : idDrop1, R(ix, y + 42, iw, 42), items, sel, L"Choose a device")) {
        ui_.outputId[slot] = ids[static_cast<size_t>(sel)];
        ctl_->SelectOutput(slot, ui_.outputId[slot]);
    }

    // One honest line about what is happening with this output.
    std::wstring detail;
    D2D1_COLOR_F dc = p.text3;
    switch (v.state) {
    case OutputUiState::Playing:
        detail = o.stats.formatValid ? o.stats.format.Describe() : L"Playing";
        break;
    case OutputUiState::Direct:
        detail = L"Windows plays this output directly (it's the default device).";
        dc = p.accent;
        break;
    case OutputUiState::Disconnected:
        detail = state_.running ? L"Disconnected — waiting for Windows to reconnect it."
                                : L"Not connected. Connect it in Bluetooth settings.";
        dc = p.warning;
        break;
    case OutputUiState::Reconnecting: detail = L"Trying to reconnect…"; dc = p.warning; break;
    case OutputUiState::Unavailable:
    case OutputUiState::Failed: detail = v.error.empty() ? L"This output is unavailable." : v.error; dc = p.error; break;
    case OutputUiState::Starting: detail = L"Opening the audio stream…"; break;
    case OutputUiState::Ready:
        detail = L"Ready";
        for (const auto& d : state_.devices)
            if (d.id == ui_.outputId[slot])
                detail = DeviceDetail(d) + (d.sampleRate ? FormatW(L" · %.1f kHz", d.sampleRate / 1000.0) : L"");
        break;
    default: detail = L"Pick the headphones or speakers for this output."; break;
    }
    c.Text(detail, R(ix + 2, y + 90, iw, 20), dc, 12);

    // Volume + mute.
    const float vy = y + 118;
    const bool muted = ui_.muted[slot];
    const D2D1_COLOR_F muteTint = p.error;
    if (gui_.IconButton(slot == 0 ? idMute0 : idMute1, R(ix - 4, vy, 36, 34), muted ? kIconMute : kIconVolume, true,
                        muted ? &muteTint : nullptr)) {
        ui_.muted[slot] = !muted;
        ctl_->SetMuted(slot, !muted);
    }
    float vol = ui_.volume[slot];
    if (gui_.Slider(slot == 0 ? idVol0 : idVol1, R(ix + 38, vy, iw - 90, 34), vol, 0.0f, 1.0f, 0.05f)) {
        ui_.volume[slot] = vol;
        ctl_->SetVolume(slot, vol);
    }
    c.Text(muted ? L"Muted" : FormatW(L"%.0f%%", ui_.volume[slot] * 100), R(ix + iw - 50, vy, 50, 34),
           muted ? p.error : p.text2, 13, DWRITE_FONT_WEIGHT_NORMAL, Align::Right);
    return H;
}

float MainWindow::DrawSyncCard(float x, float y, float w) {
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const float pad = 16, iw = w - 2 * pad, ix = x + pad;
    const bool showLatency = state_.running && state_.mode != PlaybackMode::None;
    const std::wstring caption = L"Measured up to the Windows audio driver. Earbuds add their own delay inside the "
                                 L"buds — use Auto calibrate or Fine-tune delay to line them up.";
    const float captionH = showLatency ? c.TextHeight(caption, iw, 12) : 0.0f;
    std::wstring calibMsg = state_.haveCalibration ? state_.calibration.message : L"";
    const float calibMsgH = calibMsg.empty() ? 0.0f : c.TextHeight(calibMsg, iw - 56, 12) + 20;

    // Height first (the card is drawn beneath its contents).
    float H = pad + 30 + 26;
    if (showLatency) H += 3 * 24 + 6 + captionH + 8;
    H += 40;
    if (showFineTune_) H += 2 * 50 + 30;
    H += 12 + 36;
    if (!calibMsg.empty()) H += 10 + calibMsgH;
    H += pad;
    gui_.Card(R(x, y, w, H));

    float cy = y + pad;
    c.Text(L"Sync playback", R(ix, cy, iw - 60, 30), p.text, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    bool sync = ui_.syncEnabled;
    if (gui_.Toggle(idSync, R(ix + iw - 48, cy, 48, 30), sync)) {
        ui_.syncEnabled = sync;
        ctl_->UpdateSettings([sync](Settings& s) { s.syncEnabled = sync; });
    }
    cy += 30;

    // Status chip from the real synchronization state.
    std::wstring st;
    D2D1_COLOR_F sc = p.text3;
    if (!state_.running) {
        st = ui_.syncEnabled ? L"On — starts with playback" : L"Off";
    } else if (state_.mode == PlaybackMode::Mirror) {
        st = L"Mirror mode — only one output can be adjusted";
        sc = p.accent;
    } else {
        switch (tel_.status) {
        case SyncStatus::Synchronized: st = L"In sync"; sc = p.success; break;
        case SyncStatus::Aligning: st = L"Aligning…"; sc = p.warning; break;
        case SyncStatus::SingleOutput: st = L"Only one output playing"; break;
        case SyncStatus::SyncOff: st = L"Off — outputs are not being aligned"; break;
        default: st = L"Waiting for outputs…"; break;
        }
    }
    c.FillCircle(ix + 5, cy + 13, 4, sc);
    c.Text(st, R(ix + 16, cy + 2, iw - 16, 22), p.text2, 13);
    cy += 26;

    if (showLatency) {
        cy += 6;
        for (int i = 0; i < 2; ++i) {
            const auto& o = tel_.out[i];
            std::wstring val = L"—";
            if (o.direct)
                val = L"Played by Windows";
            else if (o.rendered && o.timingValid)
                val = Ms(o.ageMs);
            KeyValue(i == 0 ? L"Output 1 delay" : L"Output 2 delay", val, ix, cy, iw);
            cy += 24;
        }
        const D2D1_COLOR_F diffColor = tel_.diffValid && std::fabs(tel_.diffMs) <= 2.0 ? p.success : p.text;
        KeyValue(L"Difference", tel_.diffValid ? Ms(std::fabs(tel_.diffMs), 1) : L"—", ix, cy, iw,
                 tel_.diffValid ? &diffColor : nullptr);
        cy += 24;
        c.Text(caption, R(ix, cy, iw, captionH), p.text3, 12, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, true);
        cy += captionH + 8;
    }

    gui_.Disclosure(idFineTune, R(ix - 8, cy, iw + 16, 36), L"Fine-tune delay", showFineTune_);
    cy += 40;
    if (showFineTune_) {
        for (int i = 0; i < 2; ++i) {
            c.Text(i == 0 ? L"Output 1" : L"Output 2", R(ix, cy, 90, 18), p.text2, 12);
            c.Text(FormatW(L"%+.0f ms", ui_.offsetMs[i]), R(ix + iw - 80, cy, 80, 18), p.text, 12,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right);
            float off = static_cast<float>(ui_.offsetMs[i]);
            if (gui_.Slider(i == 0 ? idOff0 : idOff1, R(ix, cy + 16, iw, 32), off, -500.0f, 500.0f, 5.0f)) {
                ui_.offsetMs[i] = std::round(off);
                ctl_->SetOffsetMs(i, ui_.offsetMs[i]);
            }
            cy += 50;
        }
        c.Text(L"Adds delay to one output so both are heard together.", R(ix, cy, iw - 110, 26), p.text3, 12);
        if (gui_.Link(idOffReset, R(ix + iw - 110, cy, 110, 26), L"Reset", 0, Align::Right)) {
            for (int i = 0; i < 2; ++i) {
                ui_.offsetMs[i] = 0;
                ctl_->SetOffsetMs(i, 0);
            }
        }
        cy += 30;
    }

    cy += 12;
    if (state_.calibrating) {
        const float bw = iw - 100;
        c.Fill(R(ix, cy + 6, bw, 6), p.track, 3);
        c.Fill(R(ix, cy + 6, static_cast<float>(bw * std::clamp(state_.calibFraction, 0.0, 1.0)), 6), p.accent, 3);
        c.Text(state_.calibStep, R(ix, cy + 14, bw, 22), p.text2, 12);
        if (gui_.Button(idCalibCancel, R(ix + iw - 88, cy, 88, 34), L"Cancel")) ctl_->CancelCalibration();
    } else {
        const bool canCal = ui_.micCalibration;
        if (gui_.Button(idCalibrate, R(ix, cy, 170, 36), L"Auto calibrate", ButtonKind::Secondary, canCal, kIconMic))
            ctl_->StartCalibration();
        c.Text(canCal ? L"Uses your microphone for ~10 s" : L"Turned off in Settings", R(ix + 182, cy, iw - 182, 36),
               p.text3, 12);
    }
    cy += 36;

    if (!calibMsg.empty()) {
        cy += 10;
        const bool ok = state_.calibration.success;
        const D2D1_RECT_F box = R(ix, cy, iw, calibMsgH);
        c.Fill(box, ok ? p.successBg : p.warnBg, 6);
        c.Icon(ok ? kIconCheck : kIconInfo, R(ix + 8, cy + 8, 20, 20), ok ? p.success : p.warning, 14);
        c.Text(calibMsg, R(ix + 36, cy + 10, iw - 76, calibMsgH - 20), p.text, 12, DWRITE_FONT_WEIGHT_NORMAL,
               Align::Left, true);
        if (gui_.IconButton(idCalibDismiss, R(ix + iw - 34, cy + 4, 30, 28), kIconClose)) ctl_->DismissCalibrationResult();
    }
    return H;
}

float MainWindow::DrawBanner(float x, float y, float w) {
    const Banner& b = state_.banner;
    if (b.kind == BannerKind::None || b.text.empty()) return 0;
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const float textW = w - 56;
    const float th = c.TextHeight(b.text, textW, 13);
    const float actionH = b.action == BannerAction::None ? 0.0f : 28.0f;
    const float H = std::max(48.0f, th + 26) + actionH;
    const D2D1_COLOR_F bg = b.kind == BannerKind::Error ? p.errorBg : b.kind == BannerKind::Warning ? p.warnBg : p.infoBg;
    const D2D1_COLOR_F ic = b.kind == BannerKind::Error ? p.error : b.kind == BannerKind::Warning ? p.warning : p.accent;
    c.Fill(R(x, y, w, H), bg, 8);
    c.Icon(b.kind == BannerKind::Error ? kIconError : b.kind == BannerKind::Warning ? kIconWarning : kIconInfo,
           R(x + 12, y + 13, 22, 22), ic, 16);
    c.Text(b.text, R(x + 44, y + 13, textW, th + 2), p.text, 13, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, true);
    if (b.action != BannerAction::None) {
        const bool sound = b.action == BannerAction::OpenSoundSettings;
        if (gui_.Link(idBannerAction, R(x + 44, y + H - actionH - 6, w - 56, actionH),
                      sound ? L"Open Sound settings" : L"Open Bluetooth settings"))
            Open(sound ? L"ms-settings:sound" : L"ms-settings:bluetooth");
    }
    return H;
}

float MainWindow::DrawMain(float W) {
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const float cw = W - 2 * kMargin;
    float y = Header(L"Dual Audio", W, false);

    // Overall status line.
    const bool anySelected = !ui_.outputId[0].empty() || !ui_.outputId[1].empty();
    std::wstring st;
    D2D1_COLOR_F dot = p.text3;
    if (!state_.running) {
        st = anySelected ? L"Stopped" : L"Choose your two outputs to begin";
    } else {
        dot = p.success;
        switch (state_.mode) {
        case PlaybackMode::Dual: st = L"Playing on both outputs"; break;
        case PlaybackMode::Mirror: st = L"Playing (Mirror mode)"; break;
        default: st = L"Playing on one output"; break;
        }
        if (tel_.capture.state != CaptureState::Running) {
            st = L"Waiting for audio capture…";
            dot = p.warning;
        } else {
            st += tel_.capturePeak > 0.0005f ? L" · audio detected" : L" · nothing is playing";
        }
    }
    c.FillCircle(kMargin + 5, y + 10, 4, dot);
    c.Text(st, R(kMargin + 16, y, cw - 16, 20), p.text2, 13);
    y += 32;

    y += DrawOutputCard(0, kMargin, y, cw) + 12;
    y += DrawOutputCard(1, kMargin, y, cw) + 12;
    y += DrawSyncCard(kMargin, y, cw) + 12;
    const float bh = DrawBanner(kMargin, y, cw);
    if (bh > 0) y += bh + 12;

    const bool canStart = anySelected;
    if (state_.running) {
        if (gui_.Button(idStart, R(kMargin, y, cw, 46), L"Stop", ButtonKind::Secondary, true, kIconStop)) ctl_->StopPlayback();
    } else {
        if (gui_.Button(idStart, R(kMargin, y, cw, 46), L"Start Dual Audio", ButtonKind::Primary, canStart, kIconPlay))
            ctl_->StartPlayback();
    }
    y += 46 + 10;
    if (gui_.Link(idBluetooth, R(kMargin, y, cw / 2, 34), L"Bluetooth settings", kIconBluetooth))
        Open(L"ms-settings:bluetooth");
    if (gui_.Link(idSound, R(kMargin + cw / 2, y, cw / 2, 34), L"Sound settings", kIconSpeakers, Align::Right))
        Open(L"ms-settings:sound");
    y += 34 + 16;
    return y;
}

// ---------------------------------------------------------------------------------------------
// Settings page
// ---------------------------------------------------------------------------------------------

float MainWindow::DrawSettings(float W) {
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const float cw = W - 2 * kMargin, pad = 16, iw = cw - 2 * pad, ix = kMargin + pad;
    float y = Header(L"Settings", W, true) + 4;

    auto choiceRow = [&](int id, const std::wstring& label, const std::wstring& hint,
                         const std::vector<std::wstring>& opts, int& idx, float ry) {
        c.Text(label, R(ix, ry + 10, iw, 22), p.text, 14);
        c.Text(hint, R(ix, ry + 31, iw, 18), p.text3, 12);
        return gui_.Segmented(id, R(ix, ry + 56, iw, 36), opts, idx);
    };
    auto divider = [&](float ry) { c.Line(ix, ry, ix + iw, ry, p.divider, 1); };
    constexpr float kChoiceH = 104;

    // Appearance
    y += SectionTitle(L"Appearance", kMargin, y, cw);
    gui_.Card(R(kMargin, y, cw, kChoiceH + 8));
    int theme = static_cast<int>(ui_.theme);
    if (choiceRow(idTheme, L"Theme", L"Follow Windows, or always use light or dark.", {L"System default", L"Light", L"Dark"},
                  theme, y + 4)) {
        ui_.theme = static_cast<ThemeMode>(theme);
        ctl_->UpdateSettings([theme](Settings& s) { s.theme = static_cast<ThemeMode>(theme); });
        ApplyTheme();
    }
    y += kChoiceH + 8 + 20;

    // General
    y += SectionTitle(L"General", kMargin, y, cw);
    const float gH = 3 * 58 + 16;
    gui_.Card(R(kMargin, y, cw, gH));
    float ry = y + 8;
    bool v = ui_.startMinimized;
    ToggleRow(idStartMin, L"Start minimized", L"Open DualAudio in the taskbar", v, ix, ry, iw);
    if (v != ui_.startMinimized) {
        ui_.startMinimized = v;
        ctl_->UpdateSettings([v](Settings& s) { s.startMinimized = v; });
    }
    ry += 58;
    divider(ry);
    v = ui_.autoStart;
    ToggleRow(idAutoStart, L"Start dual audio automatically", L"Begin playing to your saved outputs at launch", v, ix, ry, iw);
    if (v != ui_.autoStart) {
        ui_.autoStart = v;
        ctl_->UpdateSettings([v](Settings& s) { s.autoStart = v; });
    }
    ry += 58;
    divider(ry);
    v = ui_.runAtStartup;
    ToggleRow(idRunAtStartup, L"Open at Windows startup", L"Adds a shortcut to your Startup folder", v, ix, ry, iw);
    if (v != ui_.runAtStartup) {
        ui_.runAtStartup = v;
        ctl_->UpdateSettings([v](Settings& s) { s.runAtStartup = v; });
    }
    y += gH + 20;

    // Audio
    y += SectionTitle(L"Audio", kMargin, y, cw);
    const bool single = ui_.source == SourceMode::SingleApp;
    const float aH = 4 * kChoiceH + (single ? 52.0f : 0.0f) + 2 * 58 + 16;
    gui_.Card(R(kMargin, y, cw, aH));
    ry = y + 4;
    int src = static_cast<int>(ui_.source);
    if (choiceRow(idSource, L"Audio source", L"All apps (except DualAudio), or just one app.", {L"All apps", L"One app"},
                  src, ry)) {
        ui_.source = static_cast<SourceMode>(src);
        ctl_->UpdateSettings([src](Settings& s) { s.source = static_cast<SourceMode>(src); });
    }
    ry += kChoiceH;
    if (single) {
        std::vector<DropItem> items;
        int idx = -1;
        for (const auto& a : state_.apps) {
            items.push_back({a.displayName, a.active ? a.exeName + L" · playing" : a.exeName, kIconApp});
            if (_wcsicmp(a.exeName.c_str(), ui_.sourceApp.c_str()) == 0) idx = static_cast<int>(items.size()) - 1;
        }
        if (idx < 0 && !ui_.sourceApp.empty()) {
            items.push_back({ui_.sourceApp, L"Not playing right now", kIconApp});
            idx = static_cast<int>(items.size()) - 1;
        }
        int sel = idx;
        if (gui_.Dropdown(idApp, R(ix, ry - 6, iw - 44, 40), items, sel,
                          items.empty() ? L"No apps are playing audio" : L"Choose an app") &&
            sel >= 0 && sel < static_cast<int>(state_.apps.size())) {
            const std::wstring exe = state_.apps[static_cast<size_t>(sel)].exeName;
            ui_.sourceApp = exe;
            ctl_->UpdateSettings([exe](Settings& s) { s.sourceApp = exe; });
        }
        if (gui_.IconButton(idAppRefresh, R(ix + iw - 38, ry - 4, 36, 36), kIconRefresh)) ctl_->RefreshApps();
        ry += 52;
    }
    divider(ry);
    int buf = static_cast<int>(ui_.buffer);
    if (choiceRow(idBuffer, L"Buffer size", L"Larger is more stable; smaller has less delay.",
                  {L"Low latency", L"Balanced", L"Stable"}, buf, ry)) {
        ui_.buffer = static_cast<BufferMode>(buf);
        ctl_->UpdateSettings([buf](Settings& s) { s.buffer = static_cast<BufferMode>(buf); });
    }
    ry += kChoiceH;
    divider(ry);
    int rate = ui_.sampleRate == 44100 ? 0 : 1;
    if (choiceRow(idRate, L"Internal sample rate", L"Processing format (32-bit float, stereo).", {L"44.1 kHz", L"48 kHz"},
                  rate, ry)) {
        const int sr = rate == 0 ? 44100 : 48000;
        ui_.sampleRate = sr;
        ctl_->UpdateSettings([sr](Settings& s) { s.sampleRate = sr; });
    }
    ry += kChoiceH;
    divider(ry);
    int strength = static_cast<int>(ui_.strength);
    if (choiceRow(idStrength, L"Synchronization strength", L"How quickly clock drift is corrected (always inaudible).",
                  {L"Gentle", L"Normal", L"Strong"}, strength, ry)) {
        ui_.strength = static_cast<SyncStrength>(strength);
        ctl_->UpdateSettings([strength](Settings& s) { s.strength = static_cast<SyncStrength>(strength); });
    }
    ry += kChoiceH;
    divider(ry);
    v = ui_.clipProtection;
    ToggleRow(idClip, L"Clipping protection", L"Only acts if audio would exceed full scale", v, ix, ry, iw);
    if (v != ui_.clipProtection) {
        ui_.clipProtection = v;
        ctl_->UpdateSettings([v](Settings& s) { s.clipProtection = v; });
    }
    ry += 58;
    divider(ry);
    v = ui_.micCalibration;
    ToggleRow(idMicCal, L"Automatic calibration", L"Allow Auto calibrate to use your microphone", v, ix, ry, iw);
    if (v != ui_.micCalibration) {
        ui_.micCalibration = v;
        ctl_->UpdateSettings([v](Settings& s) { s.micCalibration = v; });
    }
    y += aH + 20;

    // Diagnostics
    y += SectionTitle(L"Diagnostics", kMargin, y, cw);
    const float dH = 2 * 58 + 60 + 16;
    gui_.Card(R(kMargin, y, cw, dH));
    ry = y + 8;
    v = ui_.showDiagnostics;
    ToggleRow(idShowDiag, L"Audio telemetry", L"Show the diagnostics panel button", v, ix, ry, iw);
    if (v != ui_.showDiagnostics) {
        ui_.showDiagnostics = v;
        ctl_->UpdateSettings([v](Settings& s) { s.showDiagnostics = v; });
    }
    ry += 58;
    divider(ry);
    v = ui_.verboseLogging;
    ToggleRow(idVerbose, L"Detailed logging", L"Write extra detail to the log files", v, ix, ry, iw);
    if (v != ui_.verboseLogging) {
        ui_.verboseLogging = v;
        ctl_->UpdateSettings([v](Settings& s) { s.verboseLogging = v; });
    }
    ry += 58;
    divider(ry);
    const float bw = (iw - 10) / 2;
    if (gui_.Button(idExport, R(ix, ry + 12, bw, 36), L"Export report", ButtonKind::Secondary, true, kIconSave)) {
        const std::wstring path = ctl_->WriteDiagnosticReport();
        if (!path.empty()) {
            const std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    if (gui_.Button(idOpenLogs, R(ix + bw + 10, ry + 12, bw, 36), L"Open logs folder", ButtonKind::Secondary, true,
                    kIconFolder))
        Open(Logger::Get().Directory().c_str());
    y += dH + 20;

    // About
    y += SectionTitle(L"About", kMargin, y, cw);
    const std::wstring about =
        L"DualAudio 1.0 plays the same audio on two outputs using only standard Windows audio APIs. It installs no "
        L"drivers, changes no system settings and needs no administrator rights; Windows keeps managing pairing, "
        L"codecs and reconnection.\n\nTwo Bluetooth devices have independent clocks and their own internal buffering, "
        L"so perfect sample-level sync is not physically possible. DualAudio continuously measures and corrects clock "
        L"drift and lets you calibrate the remaining delay difference.";
    const float abH = c.TextHeight(about, iw, 12.5f) + 2 * pad;
    gui_.Card(R(kMargin, y, cw, abH));
    c.Text(about, R(ix, y + pad, iw, abH - 2 * pad), p.text2, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, true);
    y += abH + 24;
    return y;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics page
// ---------------------------------------------------------------------------------------------

float MainWindow::DrawDiagnostics(float W) {
    const Palette& p = gui_.P();
    auto& c = canvas_;
    const float cw = W - 2 * kMargin, pad = 16, iw = cw - 2 * pad, ix = kMargin + pad;
    float y = Header(L"Diagnostics", W, true) + 4;
    const EngineTelemetry& t = tel_;

    if (!t.running) {
        c.Text(L"Start dual audio to see live telemetry.", R(kMargin, y, cw, 24), p.text2, 13);
        y += 32;
    }

    // Source
    y += SectionTitle(L"Audio source", kMargin, y, cw);
    const float sH = 7 * 22 + 2 * pad;
    gui_.Card(R(kMargin, y, cw, sH));
    float ry = y + pad;
    const wchar_t* method = state_.captureFallback ? L"Endpoint loopback"
                            : ui_.source == SourceMode::SingleApp ? L"Process loopback (one app)"
                                                                  : L"Process loopback (all apps)";
    ry += KeyValue(L"Method", method, ix, ry, iw);
    const wchar_t* cs = t.capture.state == CaptureState::Running    ? L"Running"
                        : t.capture.state == CaptureState::Retrying ? L"Retrying"
                        : t.capture.state == CaptureState::Starting ? L"Starting"
                                                                    : L"Stopped";
    ry += KeyValue(L"State", cs, ix, ry, iw);
    ry += KeyValue(L"Internal format", t.running ? FormatW(L"%.1f kHz · stereo · 32-bit float", t.timelineRate / 1000.0) : L"—", ix, ry, iw);
    ry += KeyValue(L"Packets / gaps filled", FormatW(L"%llu / %llu", t.capture.packets, t.capture.gapsFilled), ix, ry, iw);
    ry += KeyValue(L"Delivery delay", t.running ? Ms(t.capture.lastPacketDelayMs, 1) : L"—", ix, ry, iw);
    ry += KeyValue(L"Timeline clock",
                   t.captureDriftValid ? FormatW(L"%+.1f ppm vs system", t.captureClockPpm)
                                       : (t.running ? L"measuring…" : L"—"),
                   ix, ry, iw);
    c.Text(L"Level", R(ix, ry, iw * 0.48f, 22), p.text2, 13);
    const float lw = iw * 0.4f;
    const float level = std::clamp(t.capturePeak, 0.0f, 1.0f);
    c.Fill(R(ix + iw - lw, ry + 8, lw, 6), p.track, 3);
    c.Fill(R(ix + iw - lw, ry + 8, lw * level, 6), level > 0.99f ? p.error : p.accent, 3);
    y += sH + 20;

    // Outputs
    for (int i = 0; i < 2; ++i) {
        const auto& o = t.out[i];
        const OutputView& v = state_.out[i];
        y += SectionTitle(i == 0 ? L"Output 1" : L"Output 2", kMargin, y, cw);
        const int rows = 15;
        const float oH = rows * 22 + 2 * pad;
        gui_.Card(R(kMargin, y, cw, oH));
        ry = y + pad;
        ry += KeyValue(L"Device", v.name.empty() ? L"—" : v.name, ix, ry, iw);
        const D2D1_COLOR_F sc = StateColor(p, v.state);
        ry += KeyValue(L"State", OutputUiStateLabel(v.state), ix, ry, iw, &sc);
        ry += KeyValue(L"Format", o.stats.formatValid ? o.stats.format.Describe() : L"—", ix, ry, iw);
        ry += KeyValue(L"WASAPI buffer", o.stats.formatValid ? FormatW(L"%u frames (%.1f ms)", o.stats.bufferFrames, o.stats.bufferHns / 1e4) : L"—", ix, ry, iw);
        ry += KeyValue(L"Device period", o.stats.formatValid ? Ms(o.stats.periodHns / 1e4, 1) : L"—", ix, ry, iw);
        ry += KeyValue(L"Measured delay", o.timingValid ? Ms(o.ageMs, 1) : L"—", ix, ry, iw);
        ry += KeyValue(L"Target delay", o.rendered ? Ms(o.targetMs, 1) : L"—", ix, ry, iw);
        ry += KeyValue(L"Alignment error", o.timingValid ? FormatW(L"%+.2f ms", o.errorMs) : L"—", ix, ry, iw);
        ry += KeyValue(L"Rate correction", o.rendered ? FormatW(L"%+.0f ppm", o.correctionPpm) : L"—", ix, ry, iw);
        ry += KeyValue(L"Clock drift", o.driftValid ? FormatW(L"%+.1f ppm", o.driftPpm) : (o.rendered ? L"measuring…" : L"—"), ix, ry, iw);
        ry += KeyValue(L"Underruns / overruns", FormatW(L"%llu / %llu", o.stats.underrunEvents, o.stats.overrunEvents), ix, ry, iw);
        ry += KeyValue(L"Realignments", FormatW(L"%llu", o.stats.jumps), ix, ry, iw);
        ry += KeyValue(L"Transient errors", FormatW(L"%llu", o.stats.transientErrors), ix, ry, iw);
        ry += KeyValue(L"Limiter engaged", FormatW(L"%llu frames", o.stats.limitedFrames), ix, ry, iw);
        ry += KeyValue(L"Max callback gap", o.rendered ? Ms(o.maxCallbackGapMs, 1) : L"—", ix, ry, iw);
        y += oH + 20;
    }

    // Synchronization
    y += SectionTitle(L"Synchronization", kMargin, y, cw);
    const std::wstring note = L"Delays are measured with IAudioClock up to the Windows audio driver. Delay inside "
                              L"Bluetooth earbuds is not reported by Windows; it is only known after calibration.";
    const float noteH = c.TextHeight(note, iw, 12);
    const float yH = 5 * 22 + noteH + 8 + 2 * pad;
    gui_.Card(R(kMargin, y, cw, yH));
    ry = y + pad;
    ry += KeyValue(L"Status", SyncStatusLabel(t.status), ix, ry, iw);
    ry += KeyValue(L"Difference", t.diffValid ? FormatW(L"%+.2f ms", t.diffMs) : L"—", ix, ry, iw);
    ry += KeyValue(L"Base delay", t.running ? Ms(t.baseMs, 1) : L"—", ix, ry, iw);
    ry += KeyValue(L"Adaptive margin", t.running ? Ms(t.adaptiveMs, 1) : L"—", ix, ry, iw);
    static const wchar_t* strengths[] = {L"Gentle", L"Normal", L"Strong"};
    ry += KeyValue(L"Strength", strengths[static_cast<int>(ui_.strength)], ix, ry, iw);
    c.Text(note, R(ix, ry + 8, iw, noteH), p.text3, 12, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, true);
    y += yH + 16;

    if (gui_.Button(idDiagExport, R(kMargin, y, cw, 40), L"Export diagnostic report", ButtonKind::Secondary, true, kIconSave)) {
        const std::wstring path = ctl_->WriteDiagnosticReport();
        if (!path.empty()) {
            const std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    y += 40 + 24;
    return y;
}

} // namespace da::ui
