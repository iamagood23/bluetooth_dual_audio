#include "application/ApplicationController.h"

#include "common/Clock.h"
#include "common/Paths.h"
#include "diagnostics/Logger.h"

#include <algorithm>
#include <cmath>

namespace da {

namespace {
constexpr int kRetryDelaysMs[] = {1000, 3000, 8000};
constexpr int kReconnectSettleMs = 700; // let Windows finish setting up a Bluetooth endpoint
constexpr const wchar_t* kVersion = L"1.0.0";

bool IsProcessLoopbackUnsupported(HRESULT hr) {
    return hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == REGDB_E_CLASSNOTREG || hr == E_INVALIDARG ||
           hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || hr == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER) ||
           hr == AUDCLNT_E_UNSUPPORTED_FORMAT;
}

const wchar_t* SlotName(int slot) { return slot == 0 ? L"Output 1" : L"Output 2"; }
} // namespace

const wchar_t* OutputUiStateLabel(OutputUiState s) {
    switch (s) {
    case OutputUiState::Ready: return L"Ready";
    case OutputUiState::Starting: return L"Connecting…";
    case OutputUiState::Playing: return L"Playing";
    case OutputUiState::Direct: return L"Played by Windows";
    case OutputUiState::Disconnected: return L"Disconnected";
    case OutputUiState::Reconnecting: return L"Reconnecting…";
    case OutputUiState::Unavailable: return L"Unavailable";
    case OutputUiState::Failed: return L"Error";
    default: return L"Not selected";
    }
}

ApplicationController::ApplicationController() = default;

ApplicationController::~ApplicationController() { Shutdown(); }

void ApplicationController::LoadSettings() {
    settings_.Load(Settings::DefaultPath());
    settings_.runAtStartup = StartupShortcut::IsEnabled();
    Logger::Get().SetVerbose(settings_.verboseLogging);
    std::lock_guard lock(stateMu_);
    state_.settings = settings_;
}

void ApplicationController::Initialize(HWND notifyWindow, UINT notifyMessage, bool launchedAtStartup) {
    notifyWindow_ = notifyWindow;
    notifyMessage_ = notifyMessage;
    worker_ = std::thread([this, launchedAtStartup] { Run(launchedAtStartup); });
}

void ApplicationController::Shutdown() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard lock(queueMu_);
        quit_ = true;
    }
    queueCv_.notify_all();
    worker_.join();
}

AppState ApplicationController::State() const {
    notifyPending_ = false;
    std::lock_guard lock(stateMu_);
    return state_;
}

Settings ApplicationController::CurrentSettings() const {
    std::lock_guard lock(stateMu_);
    return state_.settings;
}

void ApplicationController::Post(Task t) {
    {
        std::lock_guard lock(queueMu_);
        queue_.push_back(std::move(t));
    }
    queueCv_.notify_one();
}

void ApplicationController::Schedule(int delayMs, Task t) {
    std::lock_guard lock(queueMu_);
    timers_.push_back({NowHns() + static_cast<int64_t>(delayMs) * kHnsPerMs, std::move(t)});
    queueCv_.notify_one();
}

// ---------------------------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------------------------

void ApplicationController::Run(bool launchedAtStartup) {
    ComInit com(COINIT_MULTITHREADED);
    HRESULT hr = enumerator_.Initialize();
    if (FAILED(hr)) {
        LOG_ERROR(LogChannel::Device, L"Cannot create the Windows audio device enumerator: %s", HResultToString(hr).c_str());
        banner_ = {BannerKind::Error, L"Windows audio is not available (" + HResultToString(hr) + L")."};
    } else {
        hr = monitor_.Start(enumerator_.Raw(), [this](DeviceEvent e) { Post([this, e] { OnDeviceEvent(e); }); });
        if (FAILED(hr))
            LOG_WARN(LogChannel::Device, L"Device notifications unavailable: %s", HResultToString(hr).c_str());
        RefreshDevices();
        for (int i = 0; i < 2; ++i) ApplyRouting(i);
        if (settings_.source == SourceMode::SingleApp) apps_ = ListAudioApps();
        if (settings_.autoStart) {
            LOG_APP(L"Starting dual audio automatically%s", launchedAtStartup ? L" (Windows startup)" : L"");
            DoStart();
        }
    }
    UpdateModeAndBanner();
    Publish();

    for (;;) {
        Task task;
        {
            std::unique_lock lock(queueMu_);
            for (;;) {
                if (quit_) break;
                if (!queue_.empty()) break;
                const int64_t now = NowHns();
                auto due = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.due <= now; });
                if (due != timers_.end()) {
                    task = std::move(due->fn);
                    timers_.erase(due);
                    break;
                }
                int64_t next = INT64_MAX;
                for (const auto& t : timers_) next = std::min(next, t.due);
                if (next == INT64_MAX)
                    queueCv_.wait(lock);
                else
                    queueCv_.wait_for(lock, std::chrono::microseconds((next - now) / 10 + 1));
            }
            if (quit_) break;
            if (!task && !queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (task) {
            // Crash isolation: a failing command must never take the application down.
            try {
                task();
            } catch (const std::exception& ex) {
                LOG_ERROR(LogChannel::App, L"Command failed: %S", ex.what());
            } catch (...) {
                LOG_ERROR(LogChannel::App, L"Command failed with an unknown exception");
            }
        }
        if (savePending_) {
            settings_.Save(Settings::DefaultPath());
            savePending_ = false;
        }
        UpdateModeAndBanner();
        Publish();
    }

    StopCalibrationThread();
    monitor_.Stop();
    engine_.Stop();
    settings_.Save(Settings::DefaultPath());
    LOG_APP(L"Controller stopped");
}

void ApplicationController::Publish() {
    {
        std::lock_guard lock(stateMu_);
        state_.devices = devices_;
        state_.defaultId = defaultId_;
        state_.running = running_;
        state_.mode = mode_;
        for (int i = 0; i < 2; ++i) state_.out[i] = out_[i];
        state_.captureFallback = captureFallback_;
        state_.banner = banner_;
        state_.haveCalibration = haveCalibration_;
        state_.calibration = calibration_;
        state_.apps = apps_;
        state_.settings = settings_;
    }
    if (notifyWindow_ && !notifyPending_.exchange(true)) PostMessageW(notifyWindow_, notifyMessage_, 0, 0);
}

void ApplicationController::SaveSoon() { savePending_ = true; }

const AudioDeviceInfo* ApplicationController::FindDevice(const std::wstring& id) const {
    for (const auto& d : devices_)
        if (d.id == id) return &d;
    return nullptr;
}

void ApplicationController::RefreshDevices() {
    HRESULT hr = S_OK;
    devices_ = enumerator_.ActiveRenderDevices(&hr);
    if (FAILED(hr)) LOG_WARN(LogChannel::Device, L"Device enumeration failed: %s", HResultToString(hr).c_str());
    defaultId_ = enumerator_.DefaultRenderId();
    for (int i = 0; i < 2; ++i) {
        OutputView& v = out_[i];
        v.deviceId = settings_.outputId[i];
        const AudioDeviceInfo* d = FindDevice(v.deviceId);
        v.present = d != nullptr;
        if (d) {
            v.name = d->name;
            v.kind = d->kind;
            if (settings_.outputName[i] != d->name) {
                settings_.outputName[i] = d->name;
                SaveSoon();
            }
        } else {
            v.name = settings_.outputName[i];
        }
    }
}

// Decides, for one output slot, whether DualAudio renders it, Windows plays it directly
// (it is the default device: rendering it too would double the audio), or it is waiting.
void ApplicationController::ApplyRouting(int slot) {
    OutputView& v = out_[slot];
    const std::wstring& id = settings_.outputId[slot];
    ++retryToken_[slot];
    if (!running_) {
        engine_.StopOutput(slot);
        v.error.clear();
        v.state = id.empty() ? OutputUiState::NotSelected : (v.present ? OutputUiState::Ready : OutputUiState::Disconnected);
        return;
    }
    if (id.empty()) {
        engine_.StopOutput(slot);
        engine_.SetDirect(slot, false);
        v.state = OutputUiState::NotSelected;
        return;
    }
    const bool direct = id == defaultId_;
    engine_.SetDirect(slot, direct);
    engine_.SetVolume(slot, settings_.volume[slot]);
    engine_.SetMuted(slot, settings_.muted[slot]);
    engine_.SetOffsetMs(slot, settings_.offsetMs[slot]);
    if (direct) {
        if (engine_.Output(slot).State() != OutputState::Idle) engine_.StopOutput(slot);
        if (v.state != OutputUiState::Direct)
            LOG_AUDIO(L"%s is the Windows default output: Windows plays it directly (no doubled audio)", SlotName(slot));
        v.state = OutputUiState::Direct;
        v.error.clear();
        return;
    }
    if (!v.present) {
        engine_.StopOutput(slot);
        v.state = OutputUiState::Disconnected;
        return;
    }
    const OutputState es = engine_.Output(slot).State();
    const bool sameDevice = engine_.Output(slot).DeviceId() == id;
    if (sameDevice && (es == OutputState::Running || es == OutputState::Starting)) return;
    attempts_[slot] = 0;
    StartOutputNow(slot);
}

void ApplicationController::StartOutputNow(int slot) {
    OutputView& v = out_[slot];
    const AudioDeviceInfo* d = FindDevice(settings_.outputId[slot]);
    LOG_AUDIO(L"%s: opening %s endpoint %s%s", SlotName(slot), d ? DeviceKindLabel(d->kind) : L"audio",
              ShortIdTag(settings_.outputId[slot]).c_str(), attempts_[slot] ? L" (retry)" : L"");
    engine_.StartOutput(slot, settings_.outputId[slot]);
    v.state = attempts_[slot] ? OutputUiState::Reconnecting : OutputUiState::Starting;
    v.error.clear();
}

void ApplicationController::DoStart() {
    if (running_) return;
    if (settings_.outputId[0].empty() && settings_.outputId[1].empty()) {
        banner_ = {BannerKind::Info, L"Choose at least one output first."};
        return;
    }
    EngineConfig cfg;
    cfg.capture.timelineRate = settings_.sampleRate;
    cfg.buffer = settings_.buffer;
    cfg.strength = settings_.strength;
    cfg.syncEnabled = settings_.syncEnabled;
    cfg.clipProtection = settings_.clipProtection;
    if (settings_.source == SourceMode::SingleApp) {
        const DWORD pid = FindAudioAppPid(settings_.sourceApp);
        if (!pid) {
            captureFailed_ = true;
            captureError_ = (settings_.sourceApp.empty() ? std::wstring(L"No app selected") : settings_.sourceApp +
                             L" isn't playing audio right now") +
                            L". Start playback in the app, or choose \"All apps\" in Settings.";
            return;
        }
        cfg.capture.mode = CaptureMode::SingleApp;
        cfg.capture.targetPid = pid;
    } else if (captureFallback_) {
        cfg.capture.mode = CaptureMode::Endpoint;
        cfg.capture.endpointId = defaultId_;
    }
    captureFailed_ = false;
    captureError_.clear();
    engine_.Start(cfg, [this](const EngineEvent& e) { Post([this, e] { OnEngineEvent(e); }); });
    running_ = true;
    LOG_APP(L"Dual audio started (source: %s, timeline %d Hz)",
            cfg.capture.mode == CaptureMode::AllApps ? L"all apps" :
            cfg.capture.mode == CaptureMode::SingleApp ? settings_.sourceApp.c_str() : L"default-device loopback",
            settings_.sampleRate);
    for (int i = 0; i < 2; ++i) {
        attempts_[i] = 0;
        ApplyRouting(i);
    }
}

void ApplicationController::DoStop() {
    StopCalibrationThread();
    if (!running_) return;
    engine_.Stop();
    running_ = false;
    for (int i = 0; i < 2; ++i) ApplyRouting(i);
    LOG_APP(L"Dual audio stopped");
}

void ApplicationController::Restart() {
    if (!running_) return;
    DoStop();
    DoStart();
}

void ApplicationController::OnEngineEvent(const EngineEvent& e) {
    if (!running_) return;
    switch (e.kind) {
    case EngineEventKind::OutputStarted: {
        if (e.generation != engine_.Output(e.slot).Generation()) return;
        const OutputStats s = engine_.Output(e.slot).Stats();
        attempts_[e.slot] = 0;
        out_[e.slot].state = OutputUiState::Playing;
        out_[e.slot].error.clear();
        LOG_AUDIO(L"%s connected: %s, buffer %u frames (%.1f ms)", SlotName(e.slot), s.format.Describe().c_str(),
                  s.bufferFrames, s.bufferHns / 1e4);
        break;
    }
    case EngineEventKind::OutputInitFailed:
        if (e.generation != engine_.Output(e.slot).Generation()) return;
        OnOutputFailure(e.slot, e.hr, false);
        break;
    case EngineEventKind::OutputLost:
    case EngineEventKind::OutputFormatChanged:
        if (e.generation != engine_.Output(e.slot).Generation()) return;
        if (e.kind == EngineEventKind::OutputFormatChanged) {
            LOG_AUDIO(L"%s: Windows changed the device format; reinitializing", SlotName(e.slot));
            engine_.StopOutput(e.slot);
            attempts_[e.slot] = 0;
            StartOutputNow(e.slot);
        } else {
            OnOutputFailure(e.slot, e.hr, true);
        }
        break;
    case EngineEventKind::CaptureStarted:
        captureFailed_ = false;
        captureError_.clear();
        LOG_AUDIO(L"Audio capture running");
        break;
    case EngineEventKind::CaptureLost:
        LOG_WARN(LogChannel::Audio, L"Audio capture interrupted (%s); recovering", HResultToString(e.hr).c_str());
        break;
    case EngineEventKind::CaptureOpenFailed:
        LOG_WARN(LogChannel::Audio, L"Audio capture could not start: %s", HResultToString(e.hr).c_str());
        if (settings_.source == SourceMode::AllApps && !captureFallback_ && IsProcessLoopbackUnsupported(e.hr)) {
            // MODE 2: this Windows build lacks process loopback; use classic loopback of the
            // default device instead (still documented WASAPI, still driver-free).
            LOG_WARN(LogChannel::Audio, L"Process loopback unsupported; falling back to default-device loopback");
            captureFallback_ = true;
            Restart();
        } else if (captureFallback_ && engine_.Config().capture.mode == CaptureMode::Endpoint) {
            // MODE 3: nothing driver-free works. Stop cleanly and say so.
            captureFailed_ = true;
            captureError_ = L"Dual-output audio cannot be provided using the current driver-free configuration. "
                            L"No system drivers were modified. (" + HResultToString(e.hr) + L")";
            DoStop();
        } else {
            captureFailed_ = true;
            captureError_ = L"Can't capture audio right now (" + HResultToString(e.hr) + L"). Retrying…";
        }
        break;
    }
}

void ApplicationController::OnOutputFailure(int slot, HRESULT hr, bool lost) {
    OutputView& v = out_[slot];
    engine_.StopOutput(slot); // the render thread has already exited; this releases it
    RefreshDevices();
    if (lost)
        LOG_WARN(LogChannel::Audio, L"%s disconnected (%s)", SlotName(slot), HResultToString(hr).c_str());
    else
        LOG_WARN(LogChannel::Audio, L"%s failed to start (%s)", SlotName(slot), HResultToString(hr).c_str());

    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        v.state = OutputUiState::Failed;
        v.error = L"This device uses an audio format DualAudio can't convert to.";
        return;
    }
    if (!v.present || enumerator_.State(settings_.outputId[slot]) != DEVICE_STATE_ACTIVE) {
        // Let Windows reconnect it; we resume when the endpoint comes back (device event).
        v.state = OutputUiState::Disconnected;
        v.error.clear();
        return;
    }
    if (lost) attempts_[slot] = 0;
    if (attempts_[slot] >= static_cast<int>(std::size(kRetryDelaysMs))) {
        v.state = OutputUiState::Unavailable;
        v.error = HResultToString(hr);
        LOG_WARN(LogChannel::Audio, L"%s unavailable after %d attempts; waiting for a device change", SlotName(slot),
                 attempts_[slot]);
        return;
    }
    const int delay = kRetryDelaysMs[attempts_[slot]++];
    v.state = OutputUiState::Reconnecting;
    const uint64_t token = ++retryToken_[slot];
    Schedule(delay, [this, slot, token] {
        if (!running_ || token != retryToken_[slot]) return;
        if (settings_.outputId[slot] == defaultId_) return ApplyRouting(slot);
        StartOutputNow(slot);
    });
}

void ApplicationController::OnDeviceEvent(const DeviceEvent& e) {
    static const wchar_t* names[] = {L"added", L"removed", L"state changed", L"default changed"};
    LOG_DEVICE(L"Endpoint %s %s%s", ShortIdTag(e.id).c_str(), names[static_cast<int>(e.type)],
               e.type == DeviceEventType::StateChanged
                   ? FormatW(L" -> %s", e.newState == DEVICE_STATE_ACTIVE ? L"active" : L"inactive").c_str()
                   : L"");
    const std::wstring oldDefault = defaultId_;
    RefreshDevices();

    // Windows often reports the endpoint state changes *before* the default-device change, and
    // the refresh above already sees the new default; so detect the change on any event.
    if (defaultId_ != oldDefault) {
        LOG_DEVICE(L"Windows default output changed to %s", ShortIdTag(defaultId_).c_str());
        if (running_) {
            if (engine_.Config().capture.mode == CaptureMode::Endpoint) {
                Restart(); // loopback source follows the default device
                return;
            }
            for (int i = 0; i < 2; ++i) ApplyRouting(i);
        }
    }
    if (e.type == DeviceEventType::DefaultChanged) return;

    for (int i = 0; i < 2; ++i) {
        if (settings_.outputId[i] != e.id) continue;
        OutputView& v = out_[i];
        const bool active = v.present;
        if (active && running_ &&
            (v.state == OutputUiState::Disconnected || v.state == OutputUiState::Unavailable ||
             v.state == OutputUiState::Reconnecting)) {
            LOG_DEVICE(L"%s device detected again; restoring stream", SlotName(i));
            attempts_[i] = 0;
            v.state = OutputUiState::Reconnecting;
            const uint64_t token = ++retryToken_[i];
            Schedule(kReconnectSettleMs, [this, i, token] {
                if (running_ && token == retryToken_[i]) ApplyRouting(i);
            });
        } else if (!active && (v.state == OutputUiState::Playing || v.state == OutputUiState::Starting ||
                               v.state == OutputUiState::Reconnecting)) {
            LOG_WARN(LogChannel::Device, L"%s device removed by Windows", SlotName(i));
            ++retryToken_[i];
            engine_.StopOutput(i);
            v.state = OutputUiState::Disconnected;
        } else if (!running_) {
            ApplyRouting(i);
        }
    }
}

void ApplicationController::UpdateModeAndBanner() {
    int selected = 0, direct = 0;
    for (int i = 0; i < 2; ++i) {
        if (!settings_.outputId[i].empty()) ++selected;
        if (out_[i].state == OutputUiState::Direct) ++direct;
    }
    if (!running_)
        mode_ = PlaybackMode::None;
    else if (selected == 2)
        mode_ = direct ? PlaybackMode::Mirror : PlaybackMode::Dual;
    else
        mode_ = PlaybackMode::Single;

    Banner b;
    if (captureFailed_) {
        b = {BannerKind::Error, captureError_};
    } else {
        for (int i = 0; i < 2 && b.kind == BannerKind::None; ++i) {
            const OutputView& v = out_[i];
            if (!running_) continue;
            if (v.state == OutputUiState::Disconnected)
                b = {BannerKind::Warning, FormatW(L"%s disconnected. Waiting for Windows to reconnect it…", SlotName(i)),
                     BannerAction::OpenBluetoothSettings};
            else if (v.state == OutputUiState::Reconnecting)
                b = {BannerKind::Warning, FormatW(L"%s disconnected. Trying to reconnect…", SlotName(i))};
            else if (v.state == OutputUiState::Unavailable || v.state == OutputUiState::Failed)
                b = {BannerKind::Error,
                     FormatW(L"%s unavailable. %s", SlotName(i), v.error.empty() ? L"" : v.error.c_str()),
                     BannerAction::OpenBluetoothSettings};
        }
        if (b.kind == BannerKind::None && !settings_.outputId[0].empty() && settings_.outputId[0] == settings_.outputId[1])
            b = {BannerKind::Info, L"Output 1 and Output 2 are the same device. Choose two different outputs."};
        if (b.kind == BannerKind::None && mode_ == PlaybackMode::Mirror) {
            const int d = out_[0].state == OutputUiState::Direct ? 0 : 1;
            b = {BannerKind::Info,
                 FormatW(L"Windows is playing directly to %s, so DualAudio only feeds the other output. For the tightest "
                         L"sync, set Windows' output to another device (e.g. your speakers) and mute it.",
                         out_[d].name.c_str()),
                 BannerAction::OpenSoundSettings};
        }
        if (b.kind == BannerKind::None && captureFallback_ && running_)
            b = {BannerKind::Info, L"This Windows version can't capture per-app audio, so DualAudio is copying the "
                                   L"default output device instead."};
    }
    banner_ = b;
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

void ApplicationController::SelectOutput(int slot, const std::wstring& deviceId) {
    Post([this, slot, deviceId] {
        const int other = 1 - slot;
        if (!deviceId.empty() && settings_.outputId[other] == deviceId) {
            // Picking the other slot's device swaps them.
            settings_.outputId[other] = settings_.outputId[slot];
            settings_.outputName[other] = settings_.outputName[slot];
        }
        settings_.outputId[slot] = deviceId;
        if (const AudioDeviceInfo* d = FindDevice(deviceId)) settings_.outputName[slot] = d->name;
        SaveSoon();
        RefreshDevices();
        for (int i = 0; i < 2; ++i) {
            if (running_ && engine_.Output(i).DeviceId() != settings_.outputId[i]) engine_.StopOutput(i);
            ApplyRouting(i);
        }
    });
}

void ApplicationController::StartPlayback() { Post([this] { DoStart(); }); }
void ApplicationController::StopPlayback() { Post([this] { DoStop(); }); }

void ApplicationController::SetVolume(int slot, float v) {
    Post([this, slot, v] {
        settings_.volume[slot] = std::clamp(v, 0.0f, 1.0f);
        engine_.SetVolume(slot, settings_.volume[slot]);
        SaveSoon();
    });
}

void ApplicationController::SetMuted(int slot, bool m) {
    Post([this, slot, m] {
        settings_.muted[slot] = m;
        engine_.SetMuted(slot, m);
        SaveSoon();
    });
}

void ApplicationController::SetOffsetMs(int slot, double ms) {
    Post([this, slot, ms] {
        settings_.offsetMs[slot] = std::clamp(std::round(ms), -500.0, 500.0);
        engine_.SetOffsetMs(slot, settings_.offsetMs[slot]);
        SaveSoon();
    });
}

void ApplicationController::UpdateSettings(std::function<void(Settings&)> mutate) {
    Post([this, mutate = std::move(mutate)] {
        const Settings old = settings_;
        mutate(settings_);
        Settings& s = settings_;
        if (s.runAtStartup != old.runAtStartup) {
            if (!StartupShortcut::SetEnabled(s.runAtStartup)) {
                LOG_WARN(LogChannel::App, L"Could not update the Startup shortcut");
                s.runAtStartup = StartupShortcut::IsEnabled();
            }
        }
        if (s.verboseLogging != old.verboseLogging) Logger::Get().SetVerbose(s.verboseLogging);
        if (s.clipProtection != old.clipProtection) engine_.SetClipProtection(s.clipProtection);
        if (s.strength != old.strength) engine_.SetSyncStrength(s.strength);
        if (s.syncEnabled != old.syncEnabled) {
            engine_.SetSyncEnabled(s.syncEnabled);
            LOG_SYNC(L"Synchronization %s", s.syncEnabled ? L"enabled" : L"disabled");
        }
        if (s.source == SourceMode::SingleApp && old.source != SourceMode::SingleApp) apps_ = ListAudioApps();
        const bool needsRestart = s.buffer != old.buffer || s.sampleRate != old.sampleRate || s.source != old.source ||
                                  (s.source == SourceMode::SingleApp && s.sourceApp != old.sourceApp);
        if (needsRestart) {
            if (s.source != old.source || s.sourceApp != old.sourceApp) captureFailed_ = false;
            Restart();
        }
        SaveSoon();
    });
}

void ApplicationController::RefreshApps() {
    Post([this] { apps_ = ListAudioApps(); });
}

void ApplicationController::DismissCalibrationResult() {
    Post([this] { haveCalibration_ = false; });
}

void ApplicationController::StartCalibration() {
    Post([this] {
        if (calibThread_.joinable() && !calibCancel_) return; // already running
        StopCalibrationThread();
        haveCalibration_ = false;
        auto fail = [this](const std::wstring& msg) {
            calibration_ = CalibrationResult{};
            calibration_.message = msg;
            haveCalibration_ = true;
        };
        if (!settings_.micCalibration)
            return fail(L"Microphone calibration is turned off in Settings. Use manual adjustment.");
        if (!running_ || mode_ != PlaybackMode::Dual)
            return fail(L"Automatic calibration needs both outputs playing through DualAudio. "
                        L"Start dual audio (not Mirror mode) first, or use manual adjustment.");
        for (int i = 0; i < 2; ++i)
            if (settings_.muted[i] || settings_.volume[i] < 0.2f)
                return fail(L"Unmute both outputs and set their volume above 20%, then try again.");
        calibCancel_ = false;
        {
            std::lock_guard lock(stateMu_);
            state_.calibrating = true;
            state_.calibFraction = 0.0;
            state_.calibStep = L"Starting…";
        }
        calibThread_ = std::thread([this] {
            AcousticCalibrator cal;
            const CalibrationResult r = cal.Run(
                engine_,
                [this](const std::wstring& step, double f) {
                    {
                        std::lock_guard lock(stateMu_);
                        state_.calibStep = step;
                        state_.calibFraction = f;
                    }
                    if (notifyWindow_ && !notifyPending_.exchange(true))
                        PostMessageW(notifyWindow_, notifyMessage_, 0, 0);
                },
                calibCancel_);
            Post([this, r] {
                if (calibThread_.joinable()) calibThread_.join();
                {
                    std::lock_guard lock(stateMu_);
                    state_.calibrating = false;
                }
                calibration_ = r;
                haveCalibration_ = true;
                if (r.success) {
                    for (int i = 0; i < 2; ++i) {
                        settings_.offsetMs[i] = std::clamp(r.suggestedOffsetMs[i], -500.0, 500.0);
                        engine_.SetOffsetMs(i, settings_.offsetMs[i]);
                    }
                    SaveSoon();
                }
            });
        });
    });
}

void ApplicationController::CancelCalibration() {
    calibCancel_ = true;
}

void ApplicationController::StopCalibrationThread() {
    if (!calibThread_.joinable()) return;
    calibCancel_ = true;
    calibThread_.join();
    std::lock_guard lock(stateMu_);
    state_.calibrating = false;
}

// ---------------------------------------------------------------------------------------------
// Diagnostics report
// ---------------------------------------------------------------------------------------------

std::wstring ApplicationController::WriteDiagnosticReport() const {
    const AppState s = State();
    const EngineTelemetry t = Telemetry();
    std::wstring r;
    auto line = [&](const std::wstring& x) { r += x + L"\r\n"; };

    SYSTEMTIME now;
    GetLocalTime(&now);
    line(FormatW(L"DualAudio %s diagnostic report  %04u-%02u-%02u %02u:%02u:%02u", kVersion, now.wYear, now.wMonth,
                 now.wDay, now.wHour, now.wMinute, now.wSecond));
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        OSVERSIONINFOW vi{sizeof(vi)};
        if (fn(&vi) == 0) line(FormatW(L"Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber));
    }
    line(L"");
    line(L"== System impact ==");
    line(L"Bluetooth drivers modified: NO | Audio drivers modified: NO | Kernel drivers installed: NO");
    line(L"Windows audio services modified: NO | Registry modifications: NONE | Administrator: NOT REQUIRED");
    line(L"Virtual audio driver: NOT USED | Capture: " +
         std::wstring(s.captureFallback ? L"WASAPI endpoint loopback (fallback)" : L"WASAPI process loopback"));
    line(L"");
    line(L"== Render endpoints ==");
    for (const auto& d : s.devices)
        line(FormatW(L"%s %-45s %-9s %6u Hz %u ch %s%s", d.isDefault ? L"*" : L" ", d.name.c_str(), DeviceKindLabel(d.kind),
                     d.sampleRate, d.channels, d.handsFree ? L"(hands-free) " : L"", ShortIdTag(d.id).c_str()));
    line(L"");
    line(FormatW(L"== Playback ==  running=%d  mode=%d  sync=%s  status=%s", s.running, static_cast<int>(s.mode),
                 s.settings.syncEnabled ? L"on" : L"off", SyncStatusLabel(t.status)));
    line(FormatW(L"Capture: state %d, packets %llu, gaps %llu, dropped %llu, restarts %llu, last error %s",
                 static_cast<int>(t.capture.state), t.capture.packets, t.capture.gapsFilled, t.capture.droppedFrames,
                 t.capture.restarts, HResultToString(t.capture.lastError).c_str()));
    for (int i = 0; i < 2; ++i) {
        const auto& o = t.out[i];
        const auto& v = s.out[i];
        line(FormatW(L"Output %d: %s [%s] %s", i + 1, v.name.c_str(), DeviceKindLabel(v.kind), OutputUiStateLabel(v.state)));
        if (o.stats.formatValid)
            line(FormatW(L"  format %s, buffer %u frames (%.1f ms), period %.1f ms", o.stats.format.Describe().c_str(),
                         o.stats.bufferFrames, o.stats.bufferHns / 1e4, o.stats.periodHns / 1e4));
        line(FormatW(L"  delay %.1f ms (target %.1f), error %+.2f ms, correction %+.0f ppm, drift %s",
                     o.ageMs, o.targetMs, o.errorMs, o.correctionPpm,
                     o.driftValid ? FormatW(L"%+.2f ppm", o.driftPpm).c_str() : L"n/a"));
        line(FormatW(L"  underruns %llu (%llu frames), overruns %llu, realignments %llu, transient errors %llu, "
                     L"limiter frames %llu, last error %s",
                     o.stats.underrunEvents, o.stats.underrunFrames, o.stats.overrunEvents, o.stats.jumps,
                     o.stats.transientErrors, o.stats.limitedFrames, HResultToString(o.stats.lastError).c_str()));
        line(FormatW(L"  volume %.0f%%%s, offset %+.0f ms", s.settings.volume[i] * 100, s.settings.muted[i] ? L" (muted)" : L"",
                     s.settings.offsetMs[i]));
    }
    if (t.diffValid) line(FormatW(L"Inter-output difference (Windows-reported part): %+.2f ms", t.diffMs));
    if (s.haveCalibration) line(L"Last calibration: " + s.calibration.message);
    line(L"");
    line(L"== Recent log ==");
    for (const auto& l : Logger::Get().RecentLines()) line(l);

    const std::wstring path = LogsDirectory() + FormatW(L"\\diagnostic-report-%04u%02u%02u-%02u%02u%02u.txt", now.wYear,
                                                         now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::string bytes = "\xEF\xBB\xBF" + ToUtf8(r);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    return ok ? path : std::wstring();
}

} // namespace da
