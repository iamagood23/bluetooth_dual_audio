#include "calibration/AcousticCalibrator.h"

#include "common/Clock.h"
#include "devices/DeviceEnumerator.h"
#include "diagnostics/Logger.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace da {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kChirpSeconds = 0.060;
constexpr double kF0 = 800.0, kF1 = 6000.0;
constexpr float kChirpAmplitude = 0.35f;
constexpr int kBurstsPerOutput = 5;
constexpr double kBurstPeriodSeconds = 0.8;
constexpr double kLeadInSeconds = 0.5;
constexpr double kSearchBeforeMs = 30.0;
constexpr double kSearchAfterMs = 650.0;
constexpr double kMinCorrelation = 0.25;
constexpr double kMinPeakToNoise = 6.0;
constexpr double kMaxSpreadMs = 4.0;
constexpr int kMinGoodBursts = 3;

// Exponential sine sweep with a Hann window.
std::vector<float> MakeChirp(double rate) {
    const int n = static_cast<int>(kChirpSeconds * rate);
    std::vector<float> c(static_cast<size_t>(n));
    const double k = std::log(kF1 / kF0);
    for (int i = 0; i < n; ++i) {
        const double t = i / rate;
        const double phase = 2.0 * kPi * kF0 * kChirpSeconds / k * (std::exp(t / kChirpSeconds * k) - 1.0);
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        c[static_cast<size_t>(i)] = static_cast<float>(std::sin(phase) * w);
    }
    return c;
}

// Plays chirps into one output ring at a time; everything else is silence.
class ChirpSignal final : public ITimelineSignal {
public:
    ChirpSignal(double rate, int64_t start) : chirp_(MakeChirp(rate)), start_(start) {
        period_ = static_cast<int64_t>(kBurstPeriodSeconds * rate);
    }
    int64_t BurstStart(int k) const { return start_ + k * period_; }
    static int BurstOutput(int k) { return k % 2; } // A, B, A, B, ...
    int BurstCount() const { return 2 * kBurstsPerOutput; }

    void Generate(int64_t index, uint32_t frames, float* s, int ring) noexcept override {
        for (uint32_t j = 0; j < frames; ++j) {
            float v = 0.0f;
            const int64_t rel = index + j - start_;
            if (rel >= 0) {
                const int64_t k = rel / period_;
                const int64_t off = rel - k * period_;
                if (k < BurstCount() && BurstOutput(static_cast<int>(k)) == ring &&
                    off < static_cast<int64_t>(chirp_.size()))
                    v = kChirpAmplitude * chirp_[static_cast<size_t>(off)];
            }
            s[2 * j] = v;
            s[2 * j + 1] = v;
        }
    }

private:
    std::vector<float> chirp_;
    int64_t start_;
    int64_t period_ = 1;
};

struct MicRecording {
    double rate = 0;
    std::vector<float> samples; // mono
    std::vector<std::pair<int64_t, int64_t>> stamps; // (sample index, hns)
    HRESULT hr = S_OK;
    float peak = 0;

    // Wall time of sample index s (interpolated from packet timestamps).
    bool SampleToHns(double s, int64_t& out) const {
        if (stamps.empty()) return false;
        auto it = std::upper_bound(stamps.begin(), stamps.end(), static_cast<int64_t>(s),
                                   [](int64_t v, const std::pair<int64_t, int64_t>& p) { return v < p.first; });
        if (it != stamps.begin()) --it;
        out = it->second + static_cast<int64_t>((s - static_cast<double>(it->first)) * 1e7 / rate);
        return true;
    }
    bool HnsToSample(int64_t hns, double& s) const {
        if (stamps.empty()) return false;
        auto it = std::upper_bound(stamps.begin(), stamps.end(), hns,
                                   [](int64_t v, const std::pair<int64_t, int64_t>& p) { return v < p.second; });
        if (it != stamps.begin()) --it;
        s = static_cast<double>(it->first) + (hns - it->second) * rate / 1e7;
        return true;
    }
};

void RecordMic(const std::wstring& id, int64_t untilHns, MicRecording& rec, const std::atomic<bool>& cancel) {
    ComInit com(COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    UniqueHandle ev(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDevice(id.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    WAVEFORMATEX* mixRaw = nullptr;
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mixRaw);
    CoTaskMemPtr<WAVEFORMATEX> mix(mixRaw);
    DeviceFormat fmt;
    if (SUCCEEDED(hr) && !DeviceFormat::FromWaveFormat(mix.get(), fmt)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                200000, 0, mix.get(), nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev.get());
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) {
        rec.rate = fmt.sampleRate;
        rec.samples.reserve(static_cast<size_t>(fmt.sampleRate * 14));
        hr = client->Start();
    }
    SampleReader reader;
    reader.Configure(fmt);
    std::vector<float> stereo(16384 * 2);
    while (SUCCEEDED(hr) && NowHns() < untilHns && !cancel) {
        if (WaitForSingleObject(ev.get(), 200) != WAIT_OBJECT_0) continue;
        for (;;) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            UINT64 pos = 0, qpc = 0;
            hr = cap->GetBuffer(&data, &n, &flags, &pos, &qpc);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                hr = S_OK;
                break;
            }
            if (FAILED(hr)) break;
            const uint32_t m = std::min<uint32_t>(n, 16384);
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc)
                rec.stamps.emplace_back(static_cast<int64_t>(rec.samples.size()), static_cast<int64_t>(qpc));
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                rec.samples.insert(rec.samples.end(), m, 0.0f);
            } else {
                reader.Read(data, m, stereo.data());
                for (uint32_t i = 0; i < m; ++i) {
                    const float v = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
                    rec.peak = std::max(rec.peak, std::fabs(v));
                    rec.samples.push_back(v);
                }
            }
            cap->ReleaseBuffer(n);
        }
    }
    if (client) client->Stop();
    rec.hr = hr;
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

struct Detection {
    bool found = false;
    double arrivalOffsetMs = 0; // arrival - expected device playback time
    double correlation = 0;
    double peakToNoise = 0;
};

// Normalized cross-correlation of the chirp template against the recording, around the time the
// output device reported playing it.
Detection Detect(const MicRecording& rec, const std::vector<float>& tmpl, int64_t expectedHns) {
    Detection d;
    double s0 = 0, s1 = 0;
    if (!rec.HnsToSample(expectedHns - static_cast<int64_t>(kSearchBeforeMs * kHnsPerMs), s0) ||
        !rec.HnsToSample(expectedHns + static_cast<int64_t>(kSearchAfterMs * kHnsPerMs), s1))
        return d;
    const int64_t K = static_cast<int64_t>(tmpl.size());
    const int64_t first = std::max<int64_t>(0, static_cast<int64_t>(s0));
    const int64_t last = std::min<int64_t>(static_cast<int64_t>(rec.samples.size()) - K, static_cast<int64_t>(s1));
    if (last <= first + 8) return d;

    double et = 0;
    for (float v : tmpl) et += static_cast<double>(v) * v;
    const float* x = rec.samples.data();
    double em = 0;
    for (int64_t k = 0; k < K; ++k) em += static_cast<double>(x[first + k]) * x[first + k];

    std::vector<double> rho(static_cast<size_t>(last - first + 1));
    for (int64_t lag = first; lag <= last; ++lag) {
        if (lag > first) {
            const double out = x[lag - 1], in = x[lag + K - 1];
            em = std::max(0.0, em - out * out + in * in);
        }
        double dot = 0;
        for (int64_t k = 0; k < K; ++k) dot += static_cast<double>(tmpl[static_cast<size_t>(k)]) * x[lag + k];
        rho[static_cast<size_t>(lag - first)] = (em > 1e-12) ? dot / std::sqrt(et * em) : 0.0;
    }
    size_t best = 0;
    for (size_t i = 1; i < rho.size(); ++i)
        if (rho[i] > rho[best]) best = i;
    std::vector<double> mags(rho.size());
    for (size_t i = 0; i < rho.size(); ++i) mags[i] = std::fabs(rho[i]);
    std::nth_element(mags.begin(), mags.begin() + mags.size() / 2, mags.end());
    const double noise = std::max(mags[mags.size() / 2], 1e-6);

    // Parabolic sub-sample refinement.
    double peak = static_cast<double>(best);
    if (best > 0 && best + 1 < rho.size()) {
        const double a = rho[best - 1], b = rho[best], c = rho[best + 1];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) peak += 0.5 * (a - c) / den;
    }
    int64_t arrival = 0;
    if (!rec.SampleToHns(static_cast<double>(first) + peak, arrival)) return d;
    d.correlation = rho[best];
    d.peakToNoise = rho[best] / noise;
    d.arrivalOffsetMs = HnsToMs(arrival - expectedHns);
    d.found = d.correlation >= kMinCorrelation && d.peakToNoise >= kMinPeakToNoise;
    return d;
}

} // namespace

std::wstring AcousticCalibrator::PickMicrophone(std::wstring* name) {
    DeviceEnumerator en;
    if (FAILED(en.Initialize())) return {};
    const auto mics = en.ActiveCaptureDevices();
    const AudioDeviceInfo* pick = nullptr;
    for (const auto& m : mics) {
        if (m.kind == DeviceKind::Bluetooth || m.kind == DeviceKind::Virtual) continue;
        if (!pick || m.isDefault) pick = &m;
        if (m.isDefault) break;
    }
    if (!pick) return {};
    if (name) *name = pick->name;
    return pick->id;
}

CalibrationResult AcousticCalibrator::Run(AudioEngine& engine, Progress progress, const std::atomic<bool>& cancel) {
    CalibrationResult r;
    auto report = [&](const std::wstring& s, double f) {
        if (progress) progress(s, f);
    };

    EngineTelemetry t = engine.Telemetry();
    if (!t.running || !t.out[0].rendered || !t.out[1].rendered || !t.out[0].timingValid || !t.out[1].timingValid) {
        r.message = L"Automatic calibration needs both outputs playing through DualAudio. "
                    L"(In Mirror mode Windows plays one output directly, so it cannot be measured.) "
                    L"Use manual adjustment.";
        return r;
    }
    std::wstring micName;
    const std::wstring micId = PickMicrophone(&micName);
    if (micId.empty()) {
        r.message = L"Automatic calibration unavailable: no suitable microphone. Bluetooth headset "
                    L"microphones are skipped because opening them switches earbuds to call quality. "
                    L"Use manual adjustment.";
        return r;
    }
    r.microphoneName = micName;
    LOG_SYNC(L"Calibration started (microphone: %s)", micName.c_str());

    CaptureAnchor anchor;
    if (!engine.Capture().Anchor(anchor)) {
        r.message = L"Automatic calibration unavailable: the audio timeline is not running.";
        return r;
    }
    const double rate = engine.TimelineRate();
    const int64_t start = anchor.masterIndex + static_cast<int64_t>((kLeadInSeconds + 0.1) * rate);
    ChirpSignal signal(rate, start);
    const double totalSeconds = kLeadInSeconds + signal.BurstCount() * kBurstPeriodSeconds + 1.0 +
                                std::max(t.out[0].ageMs, t.out[1].ageMs) / 1000.0;

    MicRecording rec;
    const int64_t until = NowHns() + static_cast<int64_t>(totalSeconds * kHnsPerSecond);
    std::thread mic([&] { RecordMic(micId, until, rec, cancel); });

    engine.Capture().SetSignal(&signal);
    double ageSum[2] = {0, 0};
    int ageCount[2] = {0, 0};
    while (NowHns() < until && !cancel) {
        Sleep(100);
        t = engine.Telemetry();
        for (int i = 0; i < 2; ++i)
            if (t.out[i].timingValid) {
                ageSum[i] += t.out[i].ageMs;
                ++ageCount[i];
            }
        const double left = static_cast<double>(until - NowHns()) / kHnsPerSecond;
        report(L"Listening for test tones…", std::clamp(1.0 - left / totalSeconds, 0.0, 1.0));
    }
    engine.Capture().SetSignal(nullptr);
    mic.join();
    engine.Capture().Anchor(anchor); // latest mapping (the timeline is linear)

    if (cancel) {
        r.message = L"Calibration cancelled.";
        return r;
    }
    if (FAILED(rec.hr) || rec.samples.empty()) {
        r.message = L"Automatic calibration unavailable: the microphone could not be opened (" +
                    HResultToString(rec.hr) +
                    L"). Check Windows Settings > Privacy > Microphone. Use manual adjustment.";
        LOG_WARN(LogChannel::Sync, L"Calibration mic failure: %s", HResultToString(rec.hr).c_str());
        return r;
    }
    if (ageCount[0] == 0 || ageCount[1] == 0) {
        r.message = L"Automatic calibration unavailable: output timing was not available. Use manual adjustment.";
        return r;
    }

    report(L"Analysing…", 1.0);
    const std::vector<float> tmpl = MakeChirp(rec.rate);
    std::vector<double> offsets[2];
    for (int k = 0; k < signal.BurstCount(); ++k) {
        const int o = ChirpSignal::BurstOutput(k);
        const double age = ageSum[o] / ageCount[o];
        const int64_t expected =
            MasterToHns(anchor, static_cast<double>(signal.BurstStart(k))) + static_cast<int64_t>(age * kHnsPerMs);
        const Detection d = Detect(rec, tmpl, expected);
        LOG_SYNC(L"Calibration burst %d (output %d): %s offset %.1f ms corr %.2f peak/noise %.1f", k + 1, o + 1,
                 d.found ? L"heard" : L"not heard", d.arrivalOffsetMs, d.correlation, d.peakToNoise);
        if (d.found) offsets[o].push_back(d.arrivalOffsetMs);
    }

    for (int o = 0; o < 2; ++o) {
        if (static_cast<int>(offsets[o].size()) < kMinGoodBursts) {
            r.message = FormatW(L"Automatic calibration unavailable: the microphone could not reliably hear Output %d "
                                L"(%zu of %d test tones detected). Place both earbuds next to the microphone, make "
                                L"sure they are not muted, and try again — or use manual adjustment.",
                                o + 1, offsets[o].size(), kBurstsPerOutput);
            LOG_WARN(LogChannel::Sync, L"%s", r.message.c_str());
            return r;
        }
        auto v = offsets[o];
        std::sort(v.begin(), v.end());
        r.extraLatencyMs[o] = v[v.size() / 2];
        r.spreadMs[o] = v.back() - v.front();
        if (r.spreadMs[o] > kMaxSpreadMs) {
            r.message = FormatW(L"Automatic calibration unavailable: measurements for Output %d were inconsistent "
                                L"(spread %.1f ms). Reduce background noise and try again, or use manual adjustment.",
                                o + 1, r.spreadMs[o]);
            LOG_WARN(LogChannel::Sync, L"%s", r.message.c_str());
            return r;
        }
    }

    const double maxExtra = std::max(r.extraLatencyMs[0], r.extraLatencyMs[1]);
    for (int o = 0; o < 2; ++o) r.suggestedOffsetMs[o] = std::round(maxExtra - r.extraLatencyMs[o]);
    const double diff = r.extraLatencyMs[1] - r.extraLatencyMs[0];
    r.success = true;
    r.message = std::fabs(diff) < 1.0
                    ? FormatW(L"Both outputs are heard at the same time (difference %.1f ms). No offset needed.", diff)
                    : FormatW(L"Output %d is heard %.0f ms later than Output %d. Offsets applied so both play together.",
                              diff > 0 ? 2 : 1, std::fabs(diff), diff > 0 ? 1 : 2);
    LOG_SYNC(L"Calibration succeeded: extra latency A %.1f ms (spread %.1f), B %.1f ms (spread %.1f); offsets %.0f / %.0f ms",
             r.extraLatencyMs[0], r.spreadMs[0], r.extraLatencyMs[1], r.spreadMs[1], r.suggestedOffsetMs[0],
             r.suggestedOffsetMs[1]);
    return r;
}

} // namespace da
