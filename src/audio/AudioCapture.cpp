#include "audio/AudioCapture.h"

#include "common/Clock.h"

#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>

namespace da {

namespace {

using namespace Microsoft::WRL;

// Completion handler for ActivateAudioInterfaceAsync. Must be agile (FtmBase): Windows calls it
// from a worker thread.
class ActivationHandler
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() { done_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr)); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activateHr = E_FAIL;
        ComPtr<IUnknown> unk;
        hr_ = op->GetActivateResult(&activateHr, &unk);
        if (SUCCEEDED(hr_)) hr_ = activateHr;
        if (SUCCEEDED(hr_)) hr_ = unk.As(&client_);
        SetEvent(done_.get());
        return S_OK;
    }
    HRESULT Wait(DWORD ms, ComPtr<IAudioClient>& out) {
        if (WaitForSingleObject(done_.get(), ms) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        out = client_;
        return hr_;
    }

private:
    UniqueHandle done_;
    HRESULT hr_ = E_FAIL;
    ComPtr<IAudioClient> client_;
};

constexpr int64_t kGapThresholdHns = 3 * kHnsPerMs;    // larger timestamp jumps are real gaps
constexpr double kPllGain = 0.1;                        // timeline phase-lock gain per packet
constexpr int64_t kReanchorHns = 2 * kHnsPerSecond;     // beyond this, restart the timeline clock
constexpr int64_t kIdleAfterHns = 100 * kHnsPerMs;      // no packets this long -> synthesize silence
constexpr int64_t kIdleLagHns = 40 * kHnsPerMs;         // ...up to this far behind "now"
constexpr int64_t kPreRollHns = 200 * kHnsPerMs;        // timeline history written at start
constexpr DWORD kRetryDelaysMs[] = {500, 1000, 2000, 5000, 10000};

} // namespace

AudioCapture::AudioCapture() {
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    audioEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

AudioCapture::~AudioCapture() { Stop(); }

void AudioCapture::Start(const CaptureParams& params, std::vector<AudioRingBuffer*> rings, Notify notify) {
    Stop();
    params_ = params;
    rings_ = std::move(rings);
    notify_ = std::move(notify);
    {
        std::lock_guard lock(statsMu_);
        stats_ = CaptureStats{};
        stats_.state = CaptureState::Starting;
        stats_.mode = params.mode;
    }
    ResetEvent(stopEvent_.get());

    // Pre-roll: put 160 ms of (past-timestamped) silence on the timeline before any render
    // thread can start reading, so outputs never begin with an underrun.
    scratch_.assign(4096 * 2, 0.0f);
    signalScratch_.assign(4096 * 2, 0.0f);
    const int64_t now = NowHns();
    const double preRollStart = static_cast<double>(now - kPreRollHns);
    WriteTimeline(nullptr, static_cast<uint32_t>((kPreRollHns - kIdleLagHns) * params_.timelineRate / 1e7), preRollStart);
    lastPacketWallHns_ = now - kIdleAfterHns;

    thread_ = std::thread([this] {
        try {
            ThreadMain();
        } catch (...) {
            Close();
            std::lock_guard lock(statsMu_);
            stats_.state = CaptureState::Retrying;
            stats_.lastError = E_UNEXPECTED;
            if (notify_) notify_(CaptureEvent::Lost, E_UNEXPECTED);
        }
    });
}

void AudioCapture::Stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_.get());
    thread_.join();
    std::lock_guard lock(statsMu_);
    stats_.state = CaptureState::Idle;
}

CaptureStats AudioCapture::Stats() const {
    std::lock_guard lock(statsMu_);
    return stats_;
}

void AudioCapture::SetSignal(ITimelineSignal* signal) {
    signal_.store(signal, std::memory_order_release);
    if (!thread_.joinable()) return;
    // Wait for two loop iterations so any use of the previous pointer has finished.
    const uint64_t start = loopEpoch_.load();
    for (int i = 0; i < 100 && loopEpoch_.load() < start + 2; ++i) Sleep(5);
}

HRESULT AudioCapture::Open() {
    HRESULT hr = S_OK;
    if (params_.mode == CaptureMode::Endpoint) {
        ComPtr<IMMDeviceEnumerator> en;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(hr)) hr = en->GetDevice(params_.endpointId.c_str(), &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    } else {
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        if (params_.mode == CaptureMode::AllApps) {
            ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
            ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        } else {
            ap.ProcessLoopbackParams.TargetProcessId = params_.targetPid;
            ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        }
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
        auto handler = Make<ActivationHandler>();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                         handler.Get(), &op);
        if (SUCCEEDED(hr)) hr = handler->Wait(5000, client_);
    }
    if (FAILED(hr)) return hr;

    // Ask for the internal format directly; Windows' own converter handles the source format.
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = static_cast<DWORD>(params_.timelineRate);
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * 8;
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &wf, nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = client_->SetEventHandle(audioEvent_.get()))) return hr;
    if (FAILED(hr = client_->GetService(IID_PPV_ARGS(&capture_)))) return hr;
    UINT32 bufferFrames = 0;
    client_->GetBufferSize(&bufferFrames);
    DeviceFormat::FromWaveFormat(&wf, format_);
    reader_.Configure(format_);
    const size_t frames = std::max<size_t>(bufferFrames, 4096);
    scratch_.assign(frames * 2, 0.0f);
    signalScratch_.assign(frames * 2, 0.0f);
    return client_->Start();
}

void AudioCapture::Close() {
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
}

void AudioCapture::WriteTimeline(const float* stereo, uint32_t frames, double startHns) {
    if (frames == 0 || rings_.empty()) return;
    const int64_t first = rings_[0]->WriteIndex();
    ITimelineSignal* sig = signal_.load(std::memory_order_acquire);
    uint32_t done = 0;
    const uint32_t chunk = static_cast<uint32_t>(signalScratch_.size() / 2);
    while (done < frames) {
        const uint32_t n = std::min(frames - done, chunk);
        const float* src = stereo ? stereo + static_cast<size_t>(done) * 2 : nullptr;
        for (size_t ri = 0; ri < rings_.size(); ++ri) {
            if (sig) {
                sig->Generate(first + done, n, signalScratch_.data(), static_cast<int>(ri));
                rings_[ri]->Write(signalScratch_.data(), n);
            } else if (src) {
                rings_[ri]->Write(src, n);
            } else {
                rings_[ri]->WriteSilence(n);
            }
        }
        done += n;
    }
    CaptureAnchor a;
    a.masterIndex = first;
    a.hns = static_cast<int64_t>(startHns);
    a.rate = params_.timelineRate;
    anchor_.Store(a);
    expectedNextHns_ = startHns + frames * 1e7 / params_.timelineRate;
}

void AudioCapture::IdleFill(int64_t now) {
    if (now - lastPacketWallHns_ < kIdleAfterHns) return;
    const double target = static_cast<double>(now - kIdleLagHns);
    if (target <= expectedNextHns_) return;
    const uint32_t frames =
        static_cast<uint32_t>(std::min((target - expectedNextHns_) * params_.timelineRate / 1e7, 96000.0));
    if (frames == 0) return;
    WriteTimeline(nullptr, frames, expectedNextHns_);
    std::lock_guard lock(statsMu_);
    stats_.idleFrames += frames;
}

HRESULT AudioCapture::Drain() {
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = capture_->GetNextPacketSize(&next);
        if (FAILED(hr)) return hr;
        if (next == 0) return S_OK;
        BYTE* data = nullptr;
        UINT32 n = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpc = 0;
        hr = capture_->GetBuffer(&data, &n, &flags, &devPos, &qpc);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
        if (FAILED(hr)) return hr;

        const int64_t now = NowHns();
        const bool tsValid = qpc != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
        double t = tsValid ? static_cast<double>(qpc) : expectedNextHns_;
        const uint32_t cap = static_cast<uint32_t>(scratch_.size() / 2);
        const uint32_t frames = std::min(n, cap);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            std::fill(scratch_.begin(), scratch_.begin() + static_cast<size_t>(frames) * 2, 0.0f);
        else
            reader_.Read(data, frames, scratch_.data());

        float pk = 0.0f;
        for (uint32_t i = 0; i < frames * 2; ++i) pk = std::max(pk, std::fabs(scratch_[i]));
        if (pk > peak_.load(std::memory_order_relaxed)) peak_.store(pk, std::memory_order_relaxed);

        // Phase-locked timeline. The time reference must never *step*: a step would shift the
        // measured delay of both outputs at once. So:
        //   * small deviations (timestamp jitter, the source clock's slow drift) are followed
        //     gradually (first-order PLL: steady-state lag ~75 us at 750 ppm),
        //   * a real gap (the source paused, e.g. between tracks) is filled with silence,
        //   * a packet that overlaps silence we already synthesized has the overlap dropped.
        uint32_t skip = 0;
        uint64_t inserted = 0;
        const double predicted = expectedNextHns_;
        const double gap = tsValid ? t - predicted : 0.0;
        double startHns = predicted + kPllGain * gap;
        if (gap > kReanchorHns) {
            startHns = t; // timeline clock restart (e.g. after system sleep): outputs resync
        } else if (gap > kGapThresholdHns) {
            const uint32_t sil = static_cast<uint32_t>(std::llround(gap * params_.timelineRate / 1e7));
            WriteTimeline(nullptr, sil, predicted);
            inserted = sil;
            startHns = expectedNextHns_;
        } else if (gap < -kGapThresholdHns) {
            skip = std::min<uint32_t>(frames, static_cast<uint32_t>(std::llround(-gap * params_.timelineRate / 1e7)));
            startHns = predicted;
        }
        if (frames > skip) WriteTimeline(scratch_.data() + static_cast<size_t>(skip) * 2, frames - skip, startHns);

        hr = capture_->ReleaseBuffer(n);
        lastPacketWallHns_ = now;
        {
            std::lock_guard lock(statsMu_);
            ++stats_.packets;
            stats_.frames += n;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++stats_.silentPackets;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stats_.discontinuities;
            if (inserted) {
                ++stats_.gapsFilled;
                stats_.silenceInsertedFrames += inserted;
            }
            stats_.droppedFrames += skip + (n - frames);
            if (tsValid) stats_.lastPacketDelayMs = HnsToMs(now - static_cast<int64_t>(qpc));
        }
        if (FAILED(hr)) return hr;
    }
}

void AudioCapture::ThreadMain() {
    ComInit com(COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Start() already pre-rolled the timeline (anchor, expectedNextHns_, scratch buffers).
    const int64_t start = NowHns();
    int retry = 0;
    int64_t retryAt = start;
    bool openFailedReported = false;

    for (;;) {
        int64_t now = NowHns();
        if (!client_ && now >= retryAt) {
            const HRESULT hr = com.usable() ? Open() : com.hr();
            std::lock_guard lock(statsMu_);
            if (SUCCEEDED(hr)) {
                stats_.state = CaptureState::Running;
                stats_.format = format_;
                if (everOpened_) ++stats_.restarts;
                everOpened_ = true;
                retry = 0;
                if (notify_) notify_(CaptureEvent::Started, S_OK);
            } else {
                Close();
                stats_.state = CaptureState::Retrying;
                stats_.lastError = hr;
                const DWORD delay = kRetryDelaysMs[std::min<int>(retry, _countof(kRetryDelaysMs) - 1)];
                ++retry;
                retryAt = now + static_cast<int64_t>(delay) * kHnsPerMs;
                if (!openFailedReported && notify_) notify_(CaptureEvent::OpenFailed, hr);
                openFailedReported = true;
            }
        }

        HANDLE waits[2] = {stopEvent_.get(), audioEvent_.get()};
        const DWORD w = WaitForMultipleObjects(client_ ? 2 : 1, waits, FALSE, 10);
        loopEpoch_.fetch_add(1, std::memory_order_acq_rel);
        if (w == WAIT_OBJECT_0) break;

        if (client_) {
            const HRESULT hr = Drain();
            if (FAILED(hr)) {
                Close();
                {
                    std::lock_guard lock(statsMu_);
                    stats_.state = CaptureState::Retrying;
                    stats_.lastError = hr;
                }
                retryAt = NowHns() + 500 * kHnsPerMs;
                if (notify_) notify_(CaptureEvent::Lost, hr);
            }
        }
        IdleFill(NowHns());
    }

    Close();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace da
