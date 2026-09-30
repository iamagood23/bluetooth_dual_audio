#pragma once
// Dual-output audio engine.
//
//   AudioCapture (master timeline thread) ──► ring A ──► WasapiOutput A (render thread A)
//                                         └─► ring B ──► WasapiOutput B (render thread B)
//   Sync thread: measures each output's real capture-to-playback delay via IAudioClock and the
//   capture QPC timestamps, and steers both outputs onto a common target.
//
// All public methods are called from one controller thread; telemetry is safe from any thread.
#include "audio/AudioCapture.h"
#include "audio/WasapiOutput.h"
#include "synchronization/ClockDriftEstimator.h"
#include "synchronization/LatencyCompensator.h"
#include "synchronization/SyncController.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace da {

enum class BufferMode { LowLatency = 0, Balanced, Stable };

struct EngineConfig {
    CaptureParams capture;
    BufferMode buffer = BufferMode::Balanced;
    SyncStrength strength = SyncStrength::Normal;
    bool syncEnabled = true;
    bool clipProtection = true;
};

enum class EngineEventKind {
    OutputStarted, OutputInitFailed, OutputLost, OutputFormatChanged,
    CaptureStarted, CaptureOpenFailed, CaptureLost
};

struct EngineEvent {
    EngineEventKind kind;
    int slot = -1;
    HRESULT hr = S_OK;
    uint32_t generation = 0;
};

enum class SyncStatus { Inactive, SingleOutput, Aligning, Synchronized, SyncOff };

struct OutputTelemetry {
    OutputState state = OutputState::Idle;
    bool rendered = false; // stream owned by DualAudio
    bool direct = false;   // played directly by Windows (default device)
    OutputStats stats;
    bool timingValid = false;
    double ageMs = 0;      // measured capture-to-device delay (Windows-reported part)
    double targetMs = 0;
    double errorMs = 0;    // filtered (age - target)
    double correctionPpm = 0;
    bool driftValid = false;
    double driftPpm = 0;   // device clock vs capture timeline clock
    double locked = 0;     // seconds within tolerance
    float peak = 0;
    double maxCallbackGapMs = 0;
    double extraDelayMs = 0;
};

struct EngineTelemetry {
    bool running = false;
    CaptureStats capture;
    float capturePeak = 0;
    bool captureDriftValid = false;
    double captureClockPpm = 0; // capture timeline vs system clock
    std::array<OutputTelemetry, 2> out;
    bool diffValid = false;
    double diffMs = 0;          // residual misalignment between outputs (Windows-reported part)
    SyncStatus status = SyncStatus::Inactive;
    double baseMs = 0;
    double adaptiveMs = 0;
    double timelineRate = 0;
};

class AudioEngine {
public:
    using Listener = std::function<void(const EngineEvent&)>;

    AudioEngine();
    ~AudioEngine();

    void Start(const EngineConfig& cfg, Listener listener);
    void Stop();
    bool Running() const { return running_; }
    const EngineConfig& Config() const { return cfg_; }

    void StartOutput(int slot, const std::wstring& deviceId);
    void StopOutput(int slot);
    void SetDirect(int slot, bool direct) { direct_[slot] = direct; }

    void SetVolume(int slot, float v) { outputs_[slot]->SetVolume(v); }
    void SetMuted(int slot, bool m) { outputs_[slot]->SetMuted(m); }
    void SetGate(int slot, bool audible) { outputs_[slot]->SetGate(audible); }
    void SetOffsetMs(int slot, double ms) { offsetMs_[slot] = ms; }
    void SetSyncEnabled(bool on) { syncEnabled_ = on; }
    void SetSyncStrength(SyncStrength s) { strength_ = static_cast<int>(s); }
    void SetClipProtection(bool on);

    EngineTelemetry Telemetry() const;
    AudioCapture& Capture() { return capture_; }
    WasapiOutput& Output(int slot) { return *outputs_[slot]; }
    double TimelineRate() const { return cfg_.capture.timelineRate; }
    // Controller-computed initial delay for a newly started output (ms).
    double CurrentBaseMs() const;

private:
    struct PerOutput {
        DriftController controller;
        ClockDriftEstimator deviceDrift;
        uint32_t generation = 0;
        int64_t settleUntil = 0;
        int64_t lastUpdate = 0;
        uint64_t lastUnderruns = 0;
        double pipelineMs = 0.0; // measured device pipeline depth (max-hold, slow decay)
        int64_t jumpSettleUntil = 0;
        double feedforward = 0.0; // filtered source-vs-device rate difference
    };

    void SyncThread();
    void SyncTick(int64_t now);
    static int64_t BufferRequestHns(BufferMode m);
    static double SafetyMs(BufferMode m);

    EngineConfig cfg_;
    Listener listener_;
    AudioCapture capture_;
    std::array<std::unique_ptr<WasapiOutput>, 2> outputs_;
    std::atomic<bool> running_{false};
    std::atomic<bool> direct_[2] = {false, false};
    std::atomic<double> offsetMs_[2] = {0.0, 0.0};
    std::atomic<bool> syncEnabled_{true};
    std::atomic<int> strength_{1};
    std::atomic<bool> wantOutput_[2] = {false, false};

    std::thread syncThread_;
    UniqueHandle syncStop_;
    PerOutput per_[2];
    ClockDriftEstimator captureDrift_;      // long window (display)
    ClockDriftEstimator captureDriftShort_; // 10 s window (feedforward: the source rate wanders)
    double adaptiveMs_ = 0.0;
    int64_t lastUnderrunHns_ = 0;
    int64_t lastStatusLog_ = 0;
    double captureDelayMs_ = 10.0;

    mutable std::mutex telemetryMu_;
    EngineTelemetry telemetry_;
};

const wchar_t* SyncStatusLabel(SyncStatus s);

} // namespace da
