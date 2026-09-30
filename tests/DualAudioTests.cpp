// Unit tests for the hardware-independent parts of DualAudio. Run: bin\<Config>\DualAudioTests.exe
#include "application/Settings.h"
#include "audio/AudioResampler.h"
#include "audio/AudioRingBuffer.h"
#include "audio/GainStage.h"
#include "audio/SampleFormat.h"
#include "common/SeqLock.h"
#include "synchronization/ClockDriftEstimator.h"
#include "synchronization/LatencyCompensator.h"
#include "synchronization/SyncController.h"

#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace da;

namespace {

struct Test {
    const char* name;
    std::function<void()> fn;
};
std::vector<Test>& Registry() {
    static std::vector<Test> r;
    return r;
}
struct Register {
    Register(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); }
};
int g_failures = 0;
const char* g_current = "";

#define TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(c) \
    do { if (!(c)) { ++g_failures; printf("  FAIL %s:%d  %s\n", g_current, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) \
    do { const double va_ = (a), vb_ = (b); \
         if (!(std::fabs(va_ - vb_) <= (tol))) { ++g_failures; \
             printf("  FAIL %s:%d  %s = %.6g, expected %.6g +/- %.3g\n", g_current, __LINE__, #a, va_, vb_, (double)(tol)); } } while (0)

constexpr double kPi = 3.14159265358979323846;

std::vector<float> Sine(double freq, double rate, size_t frames, double amp = 0.5) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(amp * std::sin(2 * kPi * freq * i / rate));
        v[2 * i] = s;
        v[2 * i + 1] = s;
    }
    return v;
}

double Rms(const std::vector<float>& v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(v[2 * i]) * v[2 * i];
    return std::sqrt(s / static_cast<double>(to - from));
}

// Resample `in` (stereo) with the engine's interpolator at a fixed step.
std::vector<float> Resample(const AudioResampler& rs, const std::vector<float>& in, double step, size_t outFrames) {
    std::vector<float> out(outFrames * 2, 0.0f);
    const int half = rs.HalfTaps();
    double pos = half + 1.0;
    for (size_t i = 0; i < outFrames; ++i) {
        const int64_t ip = static_cast<int64_t>(std::floor(pos));
        if (static_cast<size_t>(ip + half) >= in.size() / 2) break;
        rs.Interpolate(&in[static_cast<size_t>(ip - half + 1) * 2], pos - std::floor(pos), out[2 * i], out[2 * i + 1]);
        pos += step;
    }
    return out;
}

// Amplitude of a known frequency via single-bin DFT.
double ToneAmplitude(const std::vector<float>& v, size_t from, size_t to, double freq, double rate) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; ++i) {
        re += v[2 * i] * std::cos(2 * kPi * freq * i / rate);
        im += v[2 * i] * std::sin(2 * kPi * freq * i / rate);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

} // namespace

// ----------------------------------------------------------------------------- Ring buffer
TEST(RingBuffer_WriteReadAndWrap) {
    AudioRingBuffer rb;
    rb.Allocate(1000, 64); // rounds to 1024
    CHECK(rb.Capacity() == 1024);
    std::vector<float> frames(3000 * 2);
    for (size_t i = 0; i < 3000; ++i) frames[2 * i] = frames[2 * i + 1] = static_cast<float>(i);
    rb.Write(frames.data(), 3000);
    CHECK(rb.WriteIndex() == 3000);
    // Recent frames readable at their master index, including across the wrap point.
    for (int64_t idx : {2100, 2047, 2048, 2999}) CHECK(rb.FramePtr(idx)[0] == static_cast<float>(idx));
    // Guard mirror: 64 contiguous frames from the last slot before wrap.
    const float* p = rb.FramePtr(2047);
    for (int k = 0; k < 64; ++k) CHECK(p[2 * k] == static_cast<float>(2047 + k));
}

TEST(RingBuffer_SilenceAndSafeWindow) {
    AudioRingBuffer rb;
    rb.Allocate(4096, 256);
    rb.WriteSilence(5000);
    CHECK(rb.WriteIndex() == 5000);
    CHECK(rb.FramePtr(4999)[0] == 0.0f);
    CHECK(rb.OldestSafeIndex() == 5000 - 4096 + 256 + AudioRingBuffer::kMaxProducerChunk);
    rb.ResetTo(123);
    CHECK(rb.WriteIndex() == 123);
}

// ----------------------------------------------------------------------------- Resampler
TEST(Resampler_SameRateIsNearlyTransparent) {
    AudioResampler rs;
    rs.Configure(48000, 48000);
    CHECK(rs.SameRate());
    const auto in = Sine(1000, 48000, 20000);
    const auto out = Resample(rs, in, 1.0, 18000);
    // Integer positions: output equals input delayed by (half+1) frames, within filter ripple.
    const int d = rs.HalfTaps() + 1;
    double maxErr = 0;
    for (size_t i = 100; i < 17000; ++i) maxErr = std::max(maxErr, std::fabs(double(out[2 * i]) - in[2 * (i + d)]));
    CHECK(maxErr < 1e-3);
}

TEST(Resampler_UnityDcGainAllPhases) {
    AudioResampler rs;
    rs.Configure(48000, 44100);
    std::vector<float> dc(static_cast<size_t>(rs.Taps()) * 2, 0.8f);
    for (double f = 0; f < 1.0; f += 0.0371) {
        float l, r;
        rs.Interpolate(dc.data(), f, l, r);
        CHECK_NEAR(l, 0.8, 1e-5);
        CHECK_NEAR(r, 0.8, 1e-5);
    }
}

TEST(Resampler_48kTo44k_PreservesPassbandRejectsAliases) {
    AudioResampler rs;
    rs.Configure(48000, 44100);
    const double step = 48000.0 / 44100.0;
    // 1 kHz passes at full level.
    auto out = Resample(rs, Sine(1000, 48000, 60000), step, 50000);
    CHECK_NEAR(ToneAmplitude(out, 1000, 44100, 1000, 44100), 0.5, 0.005);
    // 18 kHz (in the passband of both rates) passes.
    out = Resample(rs, Sine(18000, 48000, 60000), step, 50000);
    CHECK_NEAR(ToneAmplitude(out, 1000, 44100, 18000, 44100), 0.5, 0.01);
    // 23 kHz cannot be represented at 44.1 kHz: it must be removed (not folded to 21.1 kHz).
    out = Resample(rs, Sine(23000, 48000, 60000), step, 50000);
    const double alias = Rms(out, 1000, 44100) / (0.5 / std::sqrt(2.0));
    printf("    23 kHz rejection: %.1f dB\n", 20 * std::log10(std::max(alias, 1e-12)));
    CHECK(alias < 0.001); // better than -60 dB
}

TEST(Resampler_DriftCorrectionShiftsRateSmoothly) {
    // Consuming 500 ppm faster raises the output frequency by 500 ppm, with no discontinuity.
    AudioResampler rs;
    rs.Configure(48000, 48000);
    const double step = 1.0 + 500e-6;
    const auto out = Resample(rs, Sine(1000, 48000, 120000), step, 110000);
    CHECK_NEAR(ToneAmplitude(out, 2000, 98000, 1000 * step, 48000), 0.5, 0.005);
    double maxJump = 0;
    for (size_t i = 2001; i < 98000; ++i) maxJump = std::max(maxJump, std::fabs(double(out[2 * i]) - out[2 * i - 2]));
    CHECK(maxJump < 0.07); // a 1 kHz 0.5-amplitude sine changes at most ~0.065 per sample
}

// ----------------------------------------------------------------------------- Gain / limiter
TEST(Gain_UnityIsBitTransparent) {
    GainStage g;
    g.Configure(48000);
    auto s = Sine(440, 48000, 4800, 0.9);
    const auto orig = s;
    g.Process(s.data(), 4800, 1.0f, true);
    CHECK(s == orig);
    CHECK(g.OversDetected() == 0);
    CHECK(g.LimiterEngagedFrames() == 0);
}

TEST(Gain_LimiterPreventsClipping) {
    GainStage g;
    g.Configure(48000);
    auto s = Sine(440, 48000, 48000, 1.6); // +4 dB over full scale
    g.Process(s.data(), 48000, 1.0f, true);
    float peak = 0;
    for (float v : s) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.0f);
    CHECK(g.OversDetected() > 0);
    CHECK(g.LimiterEngagedFrames() > 0);
}

TEST(Gain_VolumeRampsWithoutJumps) {
    GainStage g;
    g.Configure(48000);
    std::vector<float> s(4800 * 2, 1.0f);
    g.Process(s.data(), 4800, 0.0f, false);
    // Full 1 -> 0 swing takes ~10 ms (480 frames): no step larger than 1/480, no instant jump.
    double maxStep = 0;
    for (size_t i = 1; i < 4800; ++i) maxStep = std::max(maxStep, double(std::fabs(s[2 * i] - s[2 * i - 2])));
    CHECK(maxStep <= 1.0 / 480 + 1e-6);
    CHECK(s[2 * 100] > 0.7f);  // still ramping after ~2 ms
    CHECK(s[2 * 500] == 0.0f); // done by ~10.4 ms
    CHECK(s[2 * 4799] == 0.0f);
    CHECK_NEAR(GainStage::SliderToGain(0.5f), 0.25, 1e-6);
    CHECK(GainStage::SliderToGain(0.0f) == 0.0f);
}

// ----------------------------------------------------------------------------- Sample formats
TEST(SampleFormat_ParseExtensibleFloatAndPcm) {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels = 2;
    ext.Format.nSamplesPerSec = 44100;
    ext.Format.wBitsPerSample = 32;
    ext.Format.nBlockAlign = 8;
    ext.Format.nAvgBytesPerSec = 44100 * 8;
    ext.Format.cbSize = 22;
    ext.Samples.wValidBitsPerSample = 32;
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    DeviceFormat f;
    CHECK(DeviceFormat::FromWaveFormat(&ext.Format, f));
    CHECK(f.type == SampleType::Float32 && f.sampleRate == 44100 && f.channels == 2);
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    ext.Samples.wValidBitsPerSample = 24;
    CHECK(DeviceFormat::FromWaveFormat(&ext.Format, f));
    CHECK(f.type == SampleType::Int32 && f.validBits == 24);
    ext.SubFormat = GUID{}; // unknown/compressed
    CHECK(!DeviceFormat::FromWaveFormat(&ext.Format, f));
}

TEST(SampleFormat_Int16RoundTripAndClamp) {
    DeviceFormat f{48000, 2, 4, 16, SampleType::Int16};
    SampleWriter w;
    w.Configure(f);
    SampleReader r;
    r.Configure(f);
    const float in[6] = {0.5f, -0.25f, 1.5f, -1.5f, 0.0f, 0.001f};
    int16_t buf[6];
    w.Write(in, 3, reinterpret_cast<BYTE*>(buf));
    float back[6];
    r.Read(reinterpret_cast<BYTE*>(buf), 3, back);
    CHECK_NEAR(back[0], 0.5, 2.0 / 32768);
    CHECK_NEAR(back[1], -0.25, 2.0 / 32768);
    CHECK(buf[2] == 32767 && buf[3] == -32768);
    CHECK(w.ClampedSamples() == 2);
}

TEST(SampleFormat_Int24PackedAndChannelMapping) {
    DeviceFormat f24{48000, 2, 6, 24, SampleType::Int24Packed};
    SampleWriter w;
    w.Configure(f24);
    SampleReader r;
    r.Configure(f24);
    const float in[2] = {0.75f, -0.5f};
    BYTE buf[6];
    w.Write(in, 1, buf);
    float back[2];
    r.Read(buf, 1, back);
    CHECK_NEAR(back[0], 0.75, 1e-6);
    CHECK_NEAR(back[1], -0.5, 1e-6);

    // Mono device gets the average; 6-channel device keeps L/R in FL/FR, silence elsewhere.
    DeviceFormat mono{48000, 1, 4, 32, SampleType::Float32};
    w.Configure(mono);
    float m;
    const float lr[2] = {0.2f, 0.6f};
    w.Write(lr, 1, reinterpret_cast<BYTE*>(&m));
    CHECK_NEAR(m, 0.4, 1e-6);
    DeviceFormat six{48000, 6, 24, 32, SampleType::Float32};
    w.Configure(six);
    float s6[6];
    w.Write(lr, 1, reinterpret_cast<BYTE*>(s6));
    CHECK(s6[0] == 0.2f && s6[1] == 0.6f && s6[2] == 0 && s6[5] == 0);
}

// ----------------------------------------------------------------------------- Drift estimation
TEST(DriftEstimator_MeasuresPpm) {
    ClockDriftEstimator e;
    const double rate = 48000 * (1 + 37e-6); // +37 ppm
    for (int i = 0; i <= 120; ++i) {
        const int64_t t = 1'000'000'000LL + i * 5'000'000LL; // every 0.5 s
        // +/-0.2 ms position jitter, like a real IAudioClock
        const double jitter = ((i * 7919) % 11 - 5) * 0.00004 * 48000;
        e.AddSample(t, rate * (i * 0.5) + jitter);
    }
    double ppm = 0;
    CHECK(e.Ppm(48000, ppm));
    CHECK_NEAR(ppm, 37.0, 1.5);
}

TEST(DriftEstimator_ResetsOnDiscontinuity) {
    ClockDriftEstimator e;
    for (int i = 0; i < 30; ++i) e.AddSample(i * 5'000'000LL, i * 24000.0);
    CHECK(e.Count() == 30);
    e.AddSample(31 * 5'000'000LL, 10.0); // stream restarted
    CHECK(e.Count() == 1);
    double ppm;
    CHECK(!e.Ppm(48000, ppm));
}

// ----------------------------------------------------------------------------- Latency targets
TEST(Latency_BothRenderedUsesSlowestAndRelativeOffsets) {
    LatencyInputs in;
    in.rendered[0] = in.rendered[1] = true;
    in.minAgeMs[0] = 70;
    in.minAgeMs[1] = 95;
    in.offsetMs[0] = 60; // user: delay output 1 by 60 ms relative to output 2
    in.offsetMs[1] = 0;
    in.safetyMs = 15;
    const auto t = ComputeLatencyTargets(in);
    CHECK_NEAR(t.baseMs, 110, 1e-9);
    CHECK_NEAR(t.targetMs[0], 170, 1e-9);
    CHECK_NEAR(t.targetMs[1], 110, 1e-9);
    // Only the difference matters: shifting both offsets changes nothing.
    in.offsetMs[0] = -440;
    in.offsetMs[1] = -500;
    const auto t2 = ComputeLatencyTargets(in);
    CHECK_NEAR(t2.targetMs[0], 170, 1e-9);
    CHECK_NEAR(t2.targetMs[1], 110, 1e-9);
}

TEST(Latency_MirrorModeOnlyDelaysRenderedOutput) {
    LatencyInputs in;
    in.direct[0] = true;
    in.rendered[1] = true;
    in.minAgeMs[1] = 80;
    in.safetyMs = 10;
    in.offsetMs[0] = 0;
    in.offsetMs[1] = 25;
    auto t = ComputeLatencyTargets(in);
    CHECK_NEAR(t.targetMs[1], 115, 1e-9);
    in.offsetMs[1] = -50; // cannot play earlier than its own minimum
    t = ComputeLatencyTargets(in);
    CHECK_NEAR(t.targetMs[1], 90, 1e-9);
}

// ----------------------------------------------------------------------------- Sync loop
namespace {
// Closed-loop simulation: the "device" drifts by driftPpm relative to the capture clock; the
// controller's correction changes consumption speed; the error integrates accordingly.
struct SimResult {
    double finalErrorMs;
    double maxAbsCorrection;
    int jumps;
    double maxAbsErrorAfterLockMs;
};
SimResult Simulate(double initialErrorMs, double driftPpm, SyncStrength s, double seconds, double noiseMs = 0.2) {
    DriftController c;
    const SyncTuning t = TuningFor(s);
    double e = initialErrorMs / 1000.0;
    SimResult r{0, 0, 0, 0};
    const double dt = 0.05;
    unsigned seed = 12345;
    for (double time = 0; time < seconds; time += dt) {
        seed = seed * 1103515245u + 12345u;
        const double noise = ((seed >> 16) % 1000 / 1000.0 - 0.5) * 2 * noiseMs / 1000.0;
        const auto a = c.Update(e + noise, dt, true, t);
        if (a.jump) {
            e -= a.jumpSeconds;
            ++r.jumps;
            continue;
        }
        r.maxAbsCorrection = std::max(r.maxAbsCorrection, std::fabs(a.correction));
        // Delay grows when the device consumes slower than the correction compensates.
        e += (-driftPpm * 1e-6 - a.correction) * dt;
        if (time > seconds / 2) r.maxAbsErrorAfterLockMs = std::max(r.maxAbsErrorAfterLockMs, std::fabs(e) * 1000);
    }
    r.finalErrorMs = e * 1000;
    return r;
}
} // namespace

TEST(Sync_CorrectsDriftWithoutAudibleRateChanges) {
    for (SyncStrength s : {SyncStrength::Gentle, SyncStrength::Normal, SyncStrength::Strong}) {
        const auto r = Simulate(3.0, 80.0, s, 300);
        printf("    strength %d: final %+.3f ms, max |corr| %.0f ppm, jumps %d, max |err| after lock %.3f ms\n",
               static_cast<int>(s), r.finalErrorMs, r.maxAbsCorrection * 1e6, r.jumps, r.maxAbsErrorAfterLockMs);
        CHECK(std::fabs(r.finalErrorMs) < 0.5);
        CHECK(r.maxAbsErrorAfterLockMs < 1.0);
        CHECK(r.maxAbsCorrection <= TuningFor(s).maxCorrection + DriftController::kMaxTracking + 1e-12);
        CHECK(r.jumps == 0);
    }
}

TEST(Sync_TracksLargeSourceClockOffsetWithoutRepeatedJumps) {
    // Measured on real hardware: the process-loopback source ran ~755 ppm off the system clock.
    for (double drift : {-1500.0, -755.0, 755.0, 1500.0}) {
        const auto r = Simulate(0.0, drift, SyncStrength::Normal, 400);
        printf("    drift %+.0f ppm: final %+.3f ms, jumps %d, max |err| after lock %.3f ms\n", drift, r.finalErrorMs,
               r.jumps, r.maxAbsErrorAfterLockMs);
        CHECK(r.jumps <= DriftController::kMaxAcquisitionJumps); // start-up acquisition only
        CHECK(std::fabs(r.finalErrorMs) < 0.5);
        CHECK(r.maxAbsErrorAfterLockMs < 1.0);
    }
}

TEST(Sync_FeedforwardFollowsWanderingSourceClock) {
    // Source rate swings +/-1000 ppm over 60 s (seen with process loopback on a Bluetooth default
    // device). The feedforward estimate lags reality by 5 s, like the engine's 10 s window.
    auto run = [](bool useFeedforward) {
        DriftController c;
        const SyncTuning t = TuningFor(SyncStrength::Normal);
        double e = 0, maxErr = 0;
        int jumps = 0;
        for (int k = 0; k < 20 * 600; ++k) {
            const double time = k * 0.05;
            const double drift = 1000e-6 * std::sin(2 * kPi * time / 60.0);
            const double lagged = 1000e-6 * std::sin(2 * kPi * (time - 5.0) / 60.0);
            const auto a = c.Update(e, 0.05, true, t, useFeedforward ? lagged : 0.0);
            if (a.jump) {
                e -= a.jumpSeconds;
                ++jumps;
            }
            e += (drift - a.correction) * 0.05;
            if (time > 60) maxErr = std::max(maxErr, std::fabs(e));
        }
        return std::make_pair(maxErr * 1000, jumps);
    };
    const auto withFf = run(true);
    const auto without = run(false);
    printf("    wandering source: max |err| %.2f ms with feedforward (%d jumps), %.2f ms without (%d jumps)\n",
           withFf.first, withFf.second, without.first, without.second);
    CHECK(withFf.first < 2.0);
    CHECK(withFf.second == 0);
    CHECK(withFf.first < without.first);
}

TEST(Sync_JumpKeepsLearnedDrift) {
    DriftController c;
    const SyncTuning t = TuningFor(SyncStrength::Normal);
    double e = 0;
    const double drift = 900e-6;
    int jumpsAfter = 0;
    for (int k = 0; k < 20000; ++k) { // 1000 s at 20 Hz
        if (k == 10000) e += 0.080;   // sudden 80 ms disturbance (e.g. offset change)
        const auto a = c.Update(e, 0.05, true, t);
        if (a.jump) {
            e -= a.jumpSeconds;
            if (k >= 10000) ++jumpsAfter;
        }
        e += (drift - a.correction) * 0.05;
    }
    CHECK(jumpsAfter == 1);       // the disturbance is fixed by exactly one jump...
    CHECK(std::fabs(e) < 0.0005); // ...and drift tracking resumes without re-learning
}

TEST(Sync_LargeErrorUsesSingleJump) {
    const auto r = Simulate(180.0, 10.0, SyncStrength::Normal, 60);
    CHECK(r.jumps >= 1 && r.jumps <= 2);
    CHECK(std::fabs(r.finalErrorMs) < 0.5);
}

TEST(Sync_NoiseDoesNotCauseRepeatedJumps) {
    const auto r = Simulate(0.0, 20.0, SyncStrength::Strong, 600, 3.0); // noisy device (+/-3 ms)
    CHECK(r.jumps <= DriftController::kMaxAcquisitionJumps);
}

TEST(Sync_OffMeansNoCorrection) {
    DriftController c;
    const auto a = c.Update(0.010, 0.05, false, TuningFor(SyncStrength::Normal));
    CHECK(!a.jump && a.correction == 0.0);
    const auto b = c.Update(0.400, 0.05, false, TuningFor(SyncStrength::Normal)); // beyond safety limit
    (void)b;
    DriftController d;
    CHECK(d.Update(0.400, 0.05, false, TuningFor(SyncStrength::Normal)).jump);
}

// ----------------------------------------------------------------------------- Misc
TEST(SeqLock_StoresAndLoads) {
    struct S {
        int64_t a;
        double b;
        uint32_t c;
    };
    SeqLock<S> l;
    S out{};
    CHECK(!l.Load(out)); // never written
    l.Store({7, 2.5, 9});
    CHECK(l.Load(out));
    CHECK(out.a == 7 && out.b == 2.5 && out.c == 9);
}

TEST(Settings_RoundTrip) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"dualaudio-test-settings.ini";
    Settings s;
    s.theme = ThemeMode::Dark;
    s.buffer = BufferMode::Stable;
    s.sampleRate = 44100;
    s.strength = SyncStrength::Strong;
    s.outputId[0] = L"{0.0.0.00000000}.{abc}";
    s.outputName[1] = L"Galaxy Buds — ünïcode";
    s.volume[1] = 0.35f;
    s.muted[0] = true;
    s.offsetMs[1] = -120;
    s.source = SourceMode::SingleApp;
    s.sourceApp = L"spotify.exe";
    CHECK(s.Save(path));
    Settings l;
    CHECK(l.Load(path));
    CHECK(l.theme == ThemeMode::Dark && l.buffer == BufferMode::Stable && l.sampleRate == 44100);
    CHECK(l.strength == SyncStrength::Strong && l.outputId[0] == s.outputId[0] && l.outputName[1] == s.outputName[1]);
    CHECK_NEAR(l.volume[1], 0.35, 1e-3);
    CHECK(l.muted[0] && !l.muted[1] && l.offsetMs[1] == -120);
    CHECK(l.source == SourceMode::SingleApp && l.sourceApp == L"spotify.exe");
    DeleteFileW(path.c_str());
    // Missing file: defaults are kept, no crash.
    Settings d;
    CHECK(!d.Load(path));
    CHECK(d.theme == ThemeMode::System);
}

int main() {
    int run = 0;
    for (const auto& t : Registry()) {
        g_current = t.name;
        const int before = g_failures;
        printf("[ RUN  ] %s\n", t.name);
        t.fn();
        printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
        ++run;
    }
    printf("\n%d tests, %d failed checks\n", run, g_failures);
    return g_failures == 0 ? 0 : 1;
}
