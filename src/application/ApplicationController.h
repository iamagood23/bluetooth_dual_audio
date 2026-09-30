#pragma once
// Application controller: owns settings, device discovery/monitoring, the audio engine and the
// reconnection policy. Runs on its own worker thread so slow operations (Bluetooth endpoint
// activation can take seconds) never block the UI. The UI sends commands (queued) and reads
// immutable state snapshots.
#include "application/Settings.h"
#include "audio/AudioEngine.h"
#include "calibration/AcousticCalibrator.h"
#include "devices/AudioSessions.h"
#include "devices/DeviceEnumerator.h"
#include "devices/DeviceMonitor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace da {

enum class OutputUiState {
    NotSelected, Ready, Starting, Playing, Direct, Disconnected, Reconnecting, Unavailable, Failed
};
enum class PlaybackMode { None, Dual, Mirror, Single };
enum class BannerKind { None, Info, Warning, Error };
enum class BannerAction { None, OpenSoundSettings, OpenBluetoothSettings };

struct OutputView {
    std::wstring deviceId;
    std::wstring name;
    DeviceKind kind = DeviceKind::Other;
    bool present = false; // endpoint currently active in Windows
    OutputUiState state = OutputUiState::NotSelected;
    std::wstring error;
};

struct Banner {
    BannerKind kind = BannerKind::None;
    std::wstring text;
    BannerAction action = BannerAction::None;
};

struct AppState {
    std::vector<AudioDeviceInfo> devices;
    std::wstring defaultId;
    bool running = false;
    PlaybackMode mode = PlaybackMode::None;
    OutputView out[2];
    bool captureFallback = false; // endpoint loopback in use (process loopback unavailable)
    Banner banner;
    bool calibrating = false;
    double calibFraction = 0.0;
    std::wstring calibStep;
    bool haveCalibration = false;
    CalibrationResult calibration;
    std::vector<AudioAppInfo> apps;
    Settings settings;
};

class ApplicationController {
public:
    ApplicationController();
    ~ApplicationController();

    // Loads settings synchronously (call before creating the window).
    void LoadSettings();
    // Starts the worker. notifyWindow receives notifyMessage (coalesced) whenever State() changed.
    void Initialize(HWND notifyWindow, UINT notifyMessage, bool launchedAtStartup);
    void Shutdown();

    AppState State() const;
    EngineTelemetry Telemetry() const { return engine_.Telemetry(); }
    Settings CurrentSettings() const;

    // Commands (asynchronous).
    void SelectOutput(int slot, const std::wstring& deviceId);
    void StartPlayback();
    void StopPlayback();
    void SetVolume(int slot, float v);
    void SetMuted(int slot, bool muted);
    void SetOffsetMs(int slot, double ms);
    void UpdateSettings(std::function<void(Settings&)> mutate);
    void StartCalibration();
    void CancelCalibration();
    void RefreshApps();
    void DismissCalibrationResult();

    // Synchronous; safe from the UI thread. Returns the report path or empty on failure.
    std::wstring WriteDiagnosticReport() const;

private:
    using Task = std::function<void()>;
    struct Timer {
        int64_t due;
        Task fn;
    };

    void Post(Task t);
    void Schedule(int delayMs, Task t);
    void Run(bool launchedAtStartup);
    void Publish();
    void RefreshDevices();
    void ApplyRouting(int slot);
    void StartOutputNow(int slot);
    void OnEngineEvent(const EngineEvent& e);
    void OnOutputFailure(int slot, HRESULT hr, bool lost);
    void OnDeviceEvent(const DeviceEvent& e);
    void DoStart();
    void DoStop();
    void Restart();
    void StopCalibrationThread();
    void SaveSoon();
    void UpdateModeAndBanner();
    const AudioDeviceInfo* FindDevice(const std::wstring& id) const;

    // Worker-thread state.
    Settings settings_;
    DeviceEnumerator enumerator_;
    DeviceMonitor monitor_;
    AudioEngine engine_;
    std::vector<AudioDeviceInfo> devices_;
    std::wstring defaultId_;
    bool running_ = false;
    bool captureFallback_ = false;
    bool captureFailed_ = false;
    std::wstring captureError_;
    OutputView out_[2];
    int attempts_[2] = {0, 0};
    uint64_t retryToken_[2] = {0, 0};
    PlaybackMode mode_ = PlaybackMode::None;
    Banner banner_;
    std::vector<AudioAppInfo> apps_;
    bool savePending_ = false;
    std::thread calibThread_;
    std::atomic<bool> calibCancel_{false};
    bool haveCalibration_ = false;
    CalibrationResult calibration_;

    // Queue.
    std::thread worker_;
    std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::deque<Task> queue_;
    std::vector<Timer> timers_;
    bool quit_ = false;

    // Published snapshot.
    mutable std::mutex stateMu_;
    AppState state_;
    HWND notifyWindow_ = nullptr;
    UINT notifyMessage_ = 0;
    mutable std::atomic<bool> notifyPending_{false};
};

const wchar_t* OutputUiStateLabel(OutputUiState s);

} // namespace da
