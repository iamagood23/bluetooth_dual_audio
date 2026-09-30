#include "audio/WasapiOutput.h"

#include "common/Clock.h"

#include <avrt.h>
#include <algorithm>
#include <cmath>

namespace da {

// IAudioSessionEvents sink: only records why Windows disconnected the session and wakes the
// render thread. Heap-allocated and reference-counted because the session holds a reference.
class WasapiOutput::SessionEvents final : public IAudioSessionEvents {
public:
    SessionEvents(std::atomic<int>* reason, HANDLE wake) : reason_(reason), wake_(wake) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
            *ppv = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override {
        reason_->store(static_cast<int>(reason));
        SetEvent(wake_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<int>* reason_;
    HANDLE wake_;
};

namespace {
constexpr HRESULT kStalled = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

bool IsFatal(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == AUDCLNT_E_NOT_INITIALIZED || hr == E_NOTFOUND;
}
} // namespace

WasapiOutput::WasapiOutput(int slot) : slot_(slot) {
    audioEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

WasapiOutput::~WasapiOutput() { Stop(); }

void WasapiOutput::AllocateRing(uint32_t capacityFrames) {
    ring_.Allocate(capacityFrames, 2 * AudioResampler::kMaxTaps);
}

void WasapiOutput::Start(const OutputStartParams& params, Notify notify) {
    Stop();
    params_ = params;
    ResetEvent(stopEvent_.get());
    disconnectReason_ = -1;
    correction_ = 0.0;
    pendingJump_ = 0.0;
    const uint32_t gen = generation_.fetch_add(1) + 1;
    state_ = OutputState::Starting;
    thread_ = std::thread([this, params, notify = std::move(notify), gen] {
        // Crash isolation: an exception here (e.g. out of memory while allocating stream buffers)
        // fails this output only, never the application.
        try {
            ThreadMain(params, notify, gen);
        } catch (...) {
            ReleaseStream();
            lastError_ = E_UNEXPECTED;
            state_ = OutputState::Failed;
            if (notify) notify(slot_, OutputEvent::InitFailed, E_UNEXPECTED, gen);
        }
    });
}

void WasapiOutput::Stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_.get());
    thread_.join();
    if (state_ == OutputState::Running || state_ == OutputState::Starting) state_ = OutputState::Idle;
}

OutputStats WasapiOutput::Stats() const {
    OutputStats s;
    {
        std::lock_guard lock(statsMu_);
        s = stats_;
    }
    s.underrunFrames = underrunFrames_;
    s.underrunEvents = underrunEvents_;
    s.overrunEvents = overrunEvents_;
    s.jumps = jumps_;
    s.transientErrors = transientErrors_;
    s.callbacks = callbacks_;
    s.overs = overs_;
    s.limitedFrames = limited_;
    s.clampedSamples = clamped_;
    s.lastError = lastError_;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------------------------

void WasapiOutput::ThreadMain(OutputStartParams params, Notify notify, uint32_t generation) {
    ComInit com(COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HRESULT hr = com.usable() ? InitStream(params) : com.hr();
    if (FAILED(hr)) {
        lastError_ = hr;
        ReleaseStream();
        state_ = OutputState::Failed;
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (notify) notify(slot_, OutputEvent::InitFailed, hr, generation);
        return;
    }

    state_ = OutputState::Running;
    if (notify) notify(slot_, OutputEvent::Started, S_OK, generation);

    HANDLE waits[2] = {stopEvent_.get(), audioEvent_.get()};
    int timeouts = 0;
    int consecutiveErrors = 0;
    OutputEvent exitEvent = OutputEvent::Lost;
    HRESULT exitHr = S_OK;
    bool stopRequested = false;
    lastCallbackHns_ = NowHns();

    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) {
            stopRequested = true;
            break;
        }
        const int reason = disconnectReason_.load();
        if (reason >= 0) {
            exitEvent = reason == DisconnectReasonFormatChanged ? OutputEvent::FormatChanged : OutputEvent::Lost;
            exitHr = AUDCLNT_E_DEVICE_INVALIDATED;
            break;
        }
        if (w == WAIT_TIMEOUT) {
            // No buffer events for 2 s: the endpoint is stalled (e.g. a Bluetooth link that
            // dropped without Windows invalidating the stream yet). Give it one more chance.
            if (++timeouts >= 2) {
                exitHr = kStalled;
                break;
            }
            continue;
        }
        timeouts = 0;
        const int64_t now = NowHns();
        const double gapMs = HnsToMs(now - lastCallbackHns_);
        lastCallbackHns_ = now;
        if (gapMs > maxGapMs_.load(std::memory_order_relaxed)) maxGapMs_.store(gapMs, std::memory_order_relaxed);

        hr = RenderOnce(generation);
        if (SUCCEEDED(hr)) {
            consecutiveErrors = 0;
            continue;
        }
        lastError_ = hr;
        if (IsFatal(hr)) {
            exitHr = hr;
            break;
        }
        // AUDCLNT_E_BUFFER_ERROR / AUDCLNT_E_BUFFER_SIZE_ERROR and friends are usually transient
        // (e.g. a period change while Bluetooth renegotiates). Skip this cycle.
        ++transientErrors_;
        if (++consecutiveErrors > 50) {
            exitHr = hr;
            break;
        }
    }

    if (client_) client_->Stop();
    ReleaseStream();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (stopRequested) {
        state_ = OutputState::Idle;
        return;
    }
    lastError_ = exitHr;
    state_ = OutputState::Lost;
    if (notify) notify(slot_, exitEvent, exitHr, generation);
}

HRESULT WasapiOutput::InitStream(const OutputStartParams& p) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    hr = en->GetDevice(p.deviceId.c_str(), &device);
    if (FAILED(hr)) return hr;
    DWORD devState = 0;
    if (SUCCEEDED(device->GetState(&devState)) && devState != DEVICE_STATE_ACTIVE) return E_NOTFOUND;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) return hr;

    WAVEFORMATEX* mixRaw = nullptr;
    hr = client_->GetMixFormat(&mixRaw);
    if (FAILED(hr)) return hr;
    CoTaskMemPtr<WAVEFORMATEX> mix(mixRaw);
    DeviceFormat fmt;
    if (!DeviceFormat::FromWaveFormat(mix.get(), fmt)) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    // Shared mode, event driven. NOPERSIST: our session volume is never written into Windows'
    // persisted per-app volume store.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, p.bufferHns, 0,
                             mix.get(), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR || hr == AUDCLNT_E_INVALID_DEVICE_PERIOD) {
        // Let Windows choose the buffer size.
        client_.Reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (SUCCEEDED(hr))
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, 0, 0,
                                     mix.get(), nullptr);
    }
    if (FAILED(hr)) return hr;
    if (FAILED(hr = client_->SetEventHandle(audioEvent_.get()))) return hr;

    UINT32 bufferFrames = 0;
    if (FAILED(hr = client_->GetBufferSize(&bufferFrames))) return hr;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0, latency = 0;
    client_->GetDevicePeriod(&defPeriod, &minPeriod);
    client_->GetStreamLatency(&latency);

    if (FAILED(hr = client_->GetService(IID_PPV_ARGS(&render_)))) return hr;
    if (FAILED(hr = client_->GetService(IID_PPV_ARGS(&clock_)))) return hr;
    if (FAILED(hr = clock_->GetFrequency(&clockFreq_)) || clockFreq_ == 0) return FAILED(hr) ? hr : E_UNEXPECTED;
    if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&session_)))) {
        sessionEvents_ = new SessionEvents(&disconnectReason_, audioEvent_.get());
        if (FAILED(session_->RegisterAudioSessionNotification(sessionEvents_))) {
            sessionEvents_->Release();
            sessionEvents_ = nullptr;
        }
    }

    // Allocate everything the real-time loop needs, up front.
    format_ = fmt;
    scratch_.assign(static_cast<size_t>(bufferFrames) * 2, 0.0f);
    resampler_.Configure(p.timelineRate, fmt.sampleRate);
    gain_ = GainStage{};
    gain_.Configure(fmt.sampleRate);
    writer_ = SampleWriter{};
    writer_.Configure(fmt);
    baseStep_ = p.timelineRate / fmt.sampleRate;
    historyCount_ = 0;
    historyHead_ = 0;
    framesWritten_ = 0;
    fadeLeft_ = 0;
    inUnderrun_ = false;
    const int64_t bufferHns = static_cast<int64_t>(bufferFrames) * kHnsPerSecond / fmt.sampleRate;
    {
        std::lock_guard lock(statsMu_);
        stats_ = OutputStats{};
        stats_.format = fmt;
        stats_.formatValid = true;
        stats_.bufferFrames = bufferFrames;
        stats_.bufferHns = bufferHns;
        stats_.periodHns = defPeriod;
        stats_.streamLatencyHns = latency;
    }

    // Pre-roll one buffer of silence so the stream starts without a glitch.
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames, &data))) {
        render_->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        PushHistory(bufferFrames, 0, 0.0, 0.0);
    }

    // Start reading so that the first real frame reaches the device roughly initialAgeHns after
    // it was captured; the sync controller refines this within a few hundred milliseconds.
    const int64_t w = ring_.WriteIndex();
    const double ringDelay = std::max(0.0, static_cast<double>(p.initialAgeHns - bufferHns)) * p.timelineRate / 1e7;
    resyncDelayFrames_ = ringDelay;
    readPos_ = std::max(static_cast<double>(ring_.OldestSafeIndex() + 1), static_cast<double>(w) - ringDelay);
    readPos_ = std::floor(readPos_);

    return client_->Start();
}

void WasapiOutput::ReleaseStream() {
    if (session_ && sessionEvents_) session_->UnregisterAudioSessionNotification(sessionEvents_);
    if (sessionEvents_) {
        sessionEvents_->Release();
        sessionEvents_ = nullptr;
    }
    session_.Reset();
    clock_.Reset();
    render_.Reset();
    client_.Reset();
}

void WasapiOutput::PushHistory(uint32_t frames, uint32_t valid, double startPos, double step) {
    history_[historyHead_] = Block{framesWritten_, frames, valid, startPos, step};
    historyHead_ = (historyHead_ + 1) % kHistory;
    if (historyCount_ < kHistory) ++historyCount_;
    framesWritten_ += frames;
}

bool WasapiOutput::LookupMaster(double played, double& masterPos) const {
    for (int i = 1; i <= historyCount_; ++i) {
        const Block& b = history_[(historyHead_ - i + kHistory) % kHistory];
        if (played >= static_cast<double>(b.startFrame)) {
            const double off = played - static_cast<double>(b.startFrame);
            if (off >= b.frames || off >= b.valid) return false;
            masterPos = b.startPos + off * b.step;
            return true;
        }
    }
    return false;
}

void WasapiOutput::Produce(float* out, uint32_t n, uint32_t& valid, double& startPos, double& stepUsed) {
    const int64_t w = ring_.WriteIndex();
    const int64_t oldest = ring_.OldestSafeIndex();
    const int half = resampler_.HalfTaps();

    const double jump = pendingJump_.exchange(0.0, std::memory_order_acq_rel);
    if (jump != 0.0) {
        fadeFrom_ = readPos_;
        fadeLeft_ = kFadeFrames;
        readPos_ += jump;
        ++jumps_;
    }
    if (static_cast<int64_t>(std::floor(readPos_)) - half + 1 < oldest) {
        // The reader fell out of the ring's valid window (starved thread or huge jump): resync.
        readPos_ = std::floor(static_cast<double>(w) - resyncDelayFrames_);
        if (readPos_ - half + 1 < oldest) readPos_ = static_cast<double>(oldest + half);
        fadeLeft_ = 0;
        ++overrunEvents_;
    }

    const double step = baseStep_ * (1.0 + correction_.load(std::memory_order_relaxed));
    const bool bypass = resampler_.SameRate() && step == 1.0;
    stepUsed = step;
    startPos = readPos_;
    valid = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const double fl = std::floor(readPos_);
        const int64_t ip = static_cast<int64_t>(fl);
        if (ip + half >= w) break; // not enough captured audio yet: underrun
        float l, r;
        const double frac = readPos_ - fl;
        if (bypass && frac == 0.0) {
            const float* f = ring_.FramePtr(ip);
            l = f[0];
            r = f[1];
        } else {
            resampler_.Interpolate(ring_.FramePtr(ip - half + 1), frac, l, r);
        }
        if (fadeLeft_ > 0) {
            const double ffl = std::floor(fadeFrom_);
            const int64_t fp = static_cast<int64_t>(ffl);
            if (fp - half + 1 >= oldest && fp + half < w) {
                float l2, r2;
                resampler_.Interpolate(ring_.FramePtr(fp - half + 1), fadeFrom_ - ffl, l2, r2);
                const float g = static_cast<float>(fadeLeft_) / kFadeFrames; // weight of old audio
                l = l * (1.0f - g) + l2 * g;
                r = r * (1.0f - g) + r2 * g;
            }
            fadeFrom_ += step;
            --fadeLeft_;
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
        readPos_ += step;
        ++valid;
    }
    if (valid < n) {
        std::fill(out + 2 * static_cast<size_t>(valid), out + 2 * static_cast<size_t>(n), 0.0f);
        underrunFrames_ += n - valid;
        if (!inUnderrun_) ++underrunEvents_;
        inUnderrun_ = true;
    } else {
        inUnderrun_ = false;
    }
}

HRESULT WasapiOutput::RenderOnce(uint32_t generation) {
    ++callbacks_;
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return hr;
    const uint32_t bufferFrames = static_cast<uint32_t>(scratch_.size() / 2);
    const uint32_t avail = bufferFrames > padding ? bufferFrames - padding : 0;

    if (avail > 0) {
        BYTE* data = nullptr;
        hr = render_->GetBuffer(avail, &data);
        if (FAILED(hr)) return hr;
        uint32_t valid = 0;
        double startPos = 0.0, step = 1.0;
        Produce(scratch_.data(), avail, valid, startPos, step);
        const float target =
            (gate_.load(std::memory_order_relaxed) && !muted_.load(std::memory_order_relaxed))
                ? GainStage::SliderToGain(volume_.load(std::memory_order_relaxed))
                : 0.0f;
        gain_.Process(scratch_.data(), avail, target, clipProtect_.load(std::memory_order_relaxed));
        float peak = 0.0f;
        for (uint32_t i = 0; i < avail * 2; ++i) peak = std::max(peak, std::fabs(scratch_[i]));
        if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);
        writer_.Write(scratch_.data(), avail, data);
        hr = render_->ReleaseBuffer(avail, 0);
        if (FAILED(hr)) return hr;
        PushHistory(avail, valid, startPos, step);
        overs_.store(gain_.OversDetected(), std::memory_order_relaxed);
        limited_.store(gain_.LimiterEngagedFrames(), std::memory_order_relaxed);
        clamped_.store(writer_.ClampedSamples(), std::memory_order_relaxed);
    }

    // Timing: which timeline position is audible at the device right now.
    UINT64 pos = 0, qpc = 0;
    if (SUCCEEDED(clock_->GetPosition(&pos, &qpc)) && qpc != 0) {
        const double played = static_cast<double>(pos) * format_.sampleRate / static_cast<double>(clockFreq_);
        double masterPos = 0.0;
        if (LookupMaster(played, masterPos)) {
            RenderTiming t;
            t.hns = static_cast<int64_t>(qpc);
            t.masterPos = masterPos;
            t.devicePosFrames = played;
            t.paddingFrames = padding + avail;
            t.streamGeneration = generation;
            t.readPos = readPos_;
            timing_.Store(t);
        }
    }
    return S_OK;
}

} // namespace da
