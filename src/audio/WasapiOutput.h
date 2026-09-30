#pragma once
// One independent output: its own shared-mode WASAPI client, render thread, ring buffer,
// resampler, gain stage, device state and error handling.
//
// The render thread pulls from this output's ring at a fractional timeline position, so the
// synchronization controller can steer it with tiny rate corrections (ppm) and occasional
// crossfaded jumps, without ever pausing the Bluetooth stream.
#include "audio/AudioClock.h"
#include "audio/AudioResampler.h"
#include "audio/AudioRingBuffer.h"
#include "audio/GainStage.h"
#include "audio/SampleFormat.h"
#include "common/ComUtil.h"
#include "common/SeqLock.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace da {

enum class OutputState : int { Idle = 0, Starting, Running, Lost, Failed };
enum class OutputEvent { Started, InitFailed, Lost, FormatChanged };

struct OutputStartParams {
    std::wstring deviceId;
    double timelineRate = 48000.0;
    int64_t bufferHns = 400000;     // requested WASAPI buffer duration
    int64_t initialAgeHns = 900000; // desired capture-to-playback delay when the stream starts
};

struct OutputStats {
    DeviceFormat format;
    bool formatValid = false;
    uint32_t bufferFrames = 0;
    int64_t bufferHns = 0;
    int64_t periodHns = 0;
    int64_t streamLatencyHns = 0;
    uint64_t underrunFrames = 0;
    uint64_t underrunEvents = 0;
    uint64_t overrunEvents = 0;
    uint64_t jumps = 0;
    uint64_t overs = 0;
    uint64_t limitedFrames = 0;
    uint64_t clampedSamples = 0;
    uint64_t transientErrors = 0;
    uint64_t callbacks = 0;
    HRESULT lastError = S_OK;
};

class WasapiOutput {
public:
    using Notify = std::function<void(int slot, OutputEvent event, HRESULT hr, uint32_t generation)>;

    explicit WasapiOutput(int slot);
    ~WasapiOutput();
    WasapiOutput(const WasapiOutput&) = delete;
    WasapiOutput& operator=(const WasapiOutput&) = delete;

    void AllocateRing(uint32_t capacityFrames);
    AudioRingBuffer& Ring() { return ring_; }

    // Non-blocking: the render thread activates and initializes the endpoint, then reports
    // Started or InitFailed through `notify`. Stops any previous stream first.
    void Start(const OutputStartParams& params, Notify notify);
    // Blocking: stops the stream, joins the thread, releases every COM object and handle.
    void Stop();

    OutputState State() const { return state_.load(std::memory_order_acquire); }
    uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }
    const std::wstring& DeviceId() const { return params_.deviceId; } // controller thread only
    int Slot() const { return slot_; }

    // Controls: lock-free, callable from any thread.
    void SetVolume(float slider) { volume_.store(slider, std::memory_order_relaxed); }
    void SetMuted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    void SetGate(bool audible) { gate_.store(audible, std::memory_order_relaxed); }
    void SetClipProtection(bool on) { clipProtect_.store(on, std::memory_order_relaxed); }
    void SetRateCorrection(double c) { correction_.store(c, std::memory_order_relaxed); }
    double RateCorrection() const { return correction_.load(std::memory_order_relaxed); }
    void RequestJump(double deltaFrames) { pendingJump_.fetch_add(deltaFrames, std::memory_order_acq_rel); }

    bool Timing(RenderTiming& out) const { return timing_.Load(out); }
    OutputStats Stats() const;
    float TakePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    double TakeMaxCallbackGapMs() { return maxGapMs_.exchange(0.0, std::memory_order_relaxed); }

private:
    struct Block {
        int64_t startFrame;
        uint32_t frames;
        uint32_t valid;
        double startPos;
        double step;
    };
    static constexpr int kHistory = 64;
    static constexpr int kFadeFrames = 256;

    class SessionEvents;

    void ThreadMain(OutputStartParams params, Notify notify, uint32_t generation);
    HRESULT InitStream(const OutputStartParams& params);
    void ReleaseStream();
    HRESULT RenderOnce(uint32_t generation);
    void Produce(float* out, uint32_t frames, uint32_t& valid, double& startPos, double& stepUsed);
    void PushHistory(uint32_t frames, uint32_t valid, double startPos, double step);
    bool LookupMaster(double playedFrame, double& masterPos) const;

    const int slot_;
    AudioRingBuffer ring_;
    std::thread thread_;
    UniqueHandle audioEvent_;
    UniqueHandle stopEvent_;
    OutputStartParams params_;

    // Stream objects: owned and touched only by the render thread.
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<IAudioClock> clock_;
    ComPtr<IAudioSessionControl> session_;
    SessionEvents* sessionEvents_ = nullptr;
    UINT64 clockFreq_ = 0;
    DeviceFormat format_;
    AudioResampler resampler_;
    GainStage gain_;
    SampleWriter writer_;
    std::vector<float> scratch_;
    Block history_[kHistory] = {};
    int historyCount_ = 0;
    int historyHead_ = 0;
    int64_t framesWritten_ = 0;
    double readPos_ = 0.0;
    double fadeFrom_ = 0.0;
    int fadeLeft_ = 0;
    double baseStep_ = 1.0;
    double resyncDelayFrames_ = 0.0;
    bool inUnderrun_ = false;
    int64_t lastCallbackHns_ = 0;

    // Shared state.
    std::atomic<OutputState> state_{OutputState::Idle};
    std::atomic<uint32_t> generation_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> gate_{true};
    std::atomic<bool> clipProtect_{true};
    std::atomic<double> correction_{0.0};
    std::atomic<double> pendingJump_{0.0};
    std::atomic<float> peak_{0.0f};
    std::atomic<double> maxGapMs_{0.0};
    std::atomic<int> disconnectReason_{-1};
    SeqLock<RenderTiming> timing_;

    mutable std::mutex statsMu_; // guards the non-counter part of stats_ (format, sizes)
    OutputStats stats_;
    std::atomic<uint64_t> underrunFrames_{0}, underrunEvents_{0}, overrunEvents_{0}, jumps_{0},
        transientErrors_{0}, callbacks_{0}, overs_{0}, limited_{0}, clamped_{0};
    std::atomic<HRESULT> lastError_{S_OK};
};

} // namespace da
