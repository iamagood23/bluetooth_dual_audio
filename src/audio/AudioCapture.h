#pragma once
// The master timeline: captures system audio through documented WASAPI loopback mechanisms and
// appends it to every output ring with QPC timestamps.
//
//   AllApps    - process loopback, EXCLUDING DualAudio's own process tree (no feedback).
//   SingleApp  - process loopback, INCLUDING only the chosen process tree.
//   Endpoint   - classic endpoint loopback of one render device (fallback for Windows builds
//                without process loopback).
//
// No driver, no virtual device, no hooks: ActivateAudioInterfaceAsync with
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK is a documented Windows API (build 20348+).
//
// The timeline never stops while the engine runs: if nothing is playing (or the capture client
// is being re-created) the thread writes silence paced by the system clock, so the outputs stay
// continuous and synchronized.
#include "audio/AudioClock.h"
#include "audio/AudioRingBuffer.h"
#include "audio/SampleFormat.h"
#include "common/ComUtil.h"
#include "common/SeqLock.h"

#include <audioclient.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace da {

enum class CaptureMode { AllApps = 0, SingleApp, Endpoint };
enum class CaptureState { Idle = 0, Starting, Running, Retrying };
enum class CaptureEvent { Started, OpenFailed, Lost };

struct CaptureParams {
    CaptureMode mode = CaptureMode::AllApps;
    DWORD targetPid = 0;           // SingleApp
    std::wstring endpointId;       // Endpoint
    double timelineRate = 48000.0; // internal sample rate
};

// Generated audio that replaces the captured stream (used by acoustic calibration). Called once
// per output ring so a signal can target a single output.
class ITimelineSignal {
public:
    virtual ~ITimelineSignal() = default;
    virtual void Generate(int64_t masterIndex, uint32_t frames, float* stereo, int ring) noexcept = 0;
};

struct CaptureStats {
    CaptureState state = CaptureState::Idle;
    CaptureMode mode = CaptureMode::AllApps;
    HRESULT lastError = S_OK;
    DeviceFormat format;
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t silentPackets = 0;
    uint64_t discontinuities = 0;
    uint64_t gapsFilled = 0;
    uint64_t silenceInsertedFrames = 0;
    uint64_t droppedFrames = 0;
    uint64_t idleFrames = 0;
    uint64_t restarts = 0;
    double lastPacketDelayMs = 0.0; // how long after its timestamp a packet reached us
};

class AudioCapture {
public:
    using Notify = std::function<void(CaptureEvent event, HRESULT hr)>;

    AudioCapture();
    ~AudioCapture();

    void Start(const CaptureParams& params, std::vector<AudioRingBuffer*> rings, Notify notify);
    void Stop();

    bool Anchor(CaptureAnchor& out) const { return anchor_.Load(out); }
    CaptureStats Stats() const;
    float TakePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    // Replace captured audio with `signal` (nullptr restores capture). Returns once the capture
    // thread is guaranteed to have stopped using the previous signal.
    void SetSignal(ITimelineSignal* signal);

private:
    void ThreadMain();
    HRESULT Open();
    void Close();
    HRESULT Drain();
    void IdleFill(int64_t now);
    void WriteTimeline(const float* stereo, uint32_t frames, double startHns);

    CaptureParams params_;
    std::vector<AudioRingBuffer*> rings_;
    Notify notify_;
    std::thread thread_;
    UniqueHandle stopEvent_;
    UniqueHandle audioEvent_;

    // Capture thread only.
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    SampleReader reader_;
    DeviceFormat format_;
    std::vector<float> scratch_;
    std::vector<float> signalScratch_;
    double expectedNextHns_ = 0.0;
    int64_t lastPacketWallHns_ = 0;
    bool everOpened_ = false;

    std::atomic<ITimelineSignal*> signal_{nullptr};
    std::atomic<uint64_t> loopEpoch_{0};
    std::atomic<float> peak_{0.0f};
    SeqLock<CaptureAnchor> anchor_;

    mutable std::mutex statsMu_;
    CaptureStats stats_;
};

} // namespace da
