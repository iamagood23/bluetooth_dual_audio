#include "audio/AudioEngine.h"

#include "common/Clock.h"
#include "diagnostics/Logger.h"

#include <algorithm>
#include <cmath>

namespace da {

const wchar_t* SyncStatusLabel(SyncStatus s) {
    switch (s) {
    case SyncStatus::SingleOutput: return L"Single output";
    case SyncStatus::Aligning: return L"Aligning";
    case SyncStatus::Synchronized: return L"Synchronized";
    case SyncStatus::SyncOff: return L"Sync off";
    default: return L"Inactive";
    }
}

namespace {
constexpr double kRingSeconds = 2.6;          // holds base delay + up to 1 s offset difference
constexpr int64_t kSyncPeriodMs = 50;
constexpr int64_t kSettleExtraHns = 300 * kHnsPerMs;
constexpr int64_t kStaleTimingHns = 400 * kHnsPerMs;
constexpr double kMaxAdaptiveMs = 80.0;
} // namespace

AudioEngine::AudioEngine() {
    outputs_[0] = std::make_unique<WasapiOutput>(0);
    outputs_[1] = std::make_unique<WasapiOutput>(1);
    syncStop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

AudioEngine::~AudioEngine() { Stop(); }

int64_t AudioEngine::BufferRequestHns(BufferMode m) {
    switch (m) {
    case BufferMode::LowLatency: return 200000;
    case BufferMode::Stable: return 800000;
    default: return 400000;
    }
}

double AudioEngine::SafetyMs(BufferMode m) {
    switch (m) {
    case BufferMode::LowLatency: return 5.0;
    case BufferMode::Stable: return 40.0;
    default: return 15.0;
    }
}

void AudioEngine::Start(const EngineConfig& cfg, Listener listener) {
    Stop();
    cfg_ = cfg;
    listener_ = std::move(listener);
    syncEnabled_ = cfg.syncEnabled;
    strength_ = static_cast<int>(cfg.strength);
    adaptiveMs_ = 0.0;
    captureDelayMs_ = 10.0;
    captureDrift_.Reset();
    captureDriftShort_.Reset();
    captureDriftShort_.SetWindow(20);
    for (auto& p : per_) p = PerOutput{};
    {
        std::lock_guard lock(telemetryMu_);
        telemetry_ = EngineTelemetry{};
        telemetry_.timelineRate = cfg.capture.timelineRate;
    }

    const uint32_t ringFrames = static_cast<uint32_t>(cfg.capture.timelineRate * kRingSeconds);
    for (auto& o : outputs_) {
        o->AllocateRing(ringFrames);
        o->SetClipProtection(cfg.clipProtection);
    }
    LOG_AUDIO(L"Engine starting: timeline %.0f Hz float32 stereo, buffer mode %d, sync %s",
              cfg.capture.timelineRate, static_cast<int>(cfg.buffer), cfg.syncEnabled ? L"on" : L"off");

    Listener l = listener_;
    capture_.Start(cfg.capture, {&outputs_[0]->Ring(), &outputs_[1]->Ring()}, [l](CaptureEvent e, HRESULT hr) {
        EngineEvent ev{};
        ev.kind = e == CaptureEvent::Started      ? EngineEventKind::CaptureStarted
                  : e == CaptureEvent::OpenFailed ? EngineEventKind::CaptureOpenFailed
                                                  : EngineEventKind::CaptureLost;
        ev.hr = hr;
        if (l) l(ev);
    });

    ResetEvent(syncStop_.get());
    running_ = true;
    syncThread_ = std::thread([this] { SyncThread(); });
}

void AudioEngine::Stop() {
    if (!running_ && !syncThread_.joinable()) return;
    running_ = false;
    SetEvent(syncStop_.get());
    if (syncThread_.joinable()) syncThread_.join();
    for (int i = 0; i < 2; ++i) {
        wantOutput_[i] = false;
        outputs_[i]->Stop();
    }
    capture_.Stop();
    std::lock_guard lock(telemetryMu_);
    telemetry_.running = false;
    telemetry_.status = SyncStatus::Inactive;
    LOG_AUDIO(L"Engine stopped; all streams, threads and COM objects released");
}

double AudioEngine::CurrentBaseMs() const {
    std::lock_guard lock(telemetryMu_);
    if (telemetry_.baseMs > 0) return telemetry_.baseMs;
    return BufferRequestHns(cfg_.buffer) / 1e4 + 25.0 + SafetyMs(cfg_.buffer);
}

void AudioEngine::StartOutput(int slot, const std::wstring& deviceId) {
    OutputStartParams p;
    p.deviceId = deviceId;
    p.timelineRate = cfg_.capture.timelineRate;
    p.bufferHns = BufferRequestHns(cfg_.buffer);
    const double initialMs = std::max(CurrentBaseMs(), p.bufferHns / 1e4 + 20.0);
    p.initialAgeHns = static_cast<int64_t>(initialMs * kHnsPerMs);
    wantOutput_[slot] = true;
    Listener l = listener_;
    outputs_[slot]->Start(p, [l](int s, OutputEvent e, HRESULT hr, uint32_t gen) {
        EngineEvent ev{};
        switch (e) {
        case OutputEvent::Started: ev.kind = EngineEventKind::OutputStarted; break;
        case OutputEvent::InitFailed: ev.kind = EngineEventKind::OutputInitFailed; break;
        case OutputEvent::FormatChanged: ev.kind = EngineEventKind::OutputFormatChanged; break;
        default: ev.kind = EngineEventKind::OutputLost; break;
        }
        ev.slot = s;
        ev.hr = hr;
        ev.generation = gen;
        if (l) l(ev);
    });
}

void AudioEngine::StopOutput(int slot) {
    wantOutput_[slot] = false;
    outputs_[slot]->Stop();
}

void AudioEngine::SetClipProtection(bool on) {
    cfg_.clipProtection = on;
    for (auto& o : outputs_) o->SetClipProtection(on);
}

EngineTelemetry AudioEngine::Telemetry() const {
    std::lock_guard lock(telemetryMu_);
    return telemetry_;
}

void AudioEngine::SyncThread() {
    ComInit com(COINIT_MULTITHREADED);
    LOG_SYNC(L"Synchronization controller started");
    while (WaitForSingleObject(syncStop_.get(), static_cast<DWORD>(kSyncPeriodMs)) == WAIT_TIMEOUT) {
        SyncTick(NowHns());
    }
    LOG_SYNC(L"Synchronization controller stopped");
}

void AudioEngine::SyncTick(int64_t now) {
    const double rate = cfg_.capture.timelineRate;
    const bool syncOn = syncEnabled_;
    const SyncTuning tuning = TuningFor(static_cast<SyncStrength>(strength_.load()));

    EngineTelemetry t;
    t.running = true;
    t.timelineRate = rate;
    t.capture = capture_.Stats();
    {
        std::lock_guard lock(telemetryMu_);
        t.capturePeak = std::max(capture_.TakePeak(), telemetry_.capturePeak * 0.7f);
        for (int i = 0; i < 2; ++i) t.out[i].peak = telemetry_.out[i].peak * 0.7f;
    }

    CaptureAnchor anchor;
    const bool haveAnchor = capture_.Anchor(anchor);
    if (t.capture.lastPacketDelayMs > 0 && t.capture.lastPacketDelayMs < 500)
        captureDelayMs_ += 0.05 * (t.capture.lastPacketDelayMs - captureDelayMs_);
    if (haveAnchor) {
        captureDrift_.AddSample(anchor.hns, static_cast<double>(anchor.masterIndex));
        captureDriftShort_.AddSample(anchor.hns, static_cast<double>(anchor.masterIndex));
    }
    double capPpm = 0, capShortPpm = 0;
    t.captureDriftValid = captureDrift_.Ppm(rate, capPpm, 20.0);
    t.captureClockPpm = capPpm;
    const bool capShortValid = captureDriftShort_.Ppm(rate, capShortPpm, 5.0);

    // Latency targets.
    LatencyInputs in;
    OutputStats stats[2];
    OutputState states[2];
    for (int i = 0; i < 2; ++i) {
        states[i] = outputs_[i]->State();
        stats[i] = outputs_[i]->Stats();
        in.rendered[i] = wantOutput_[i] && states[i] == OutputState::Running;
        in.direct[i] = direct_[i];
        // Smallest sustainable delay: audio queued in the device pipeline (measured; falls back to
        // the WASAPI buffer size until known) + capture delivery delay + one capture packet +
        // resampler look-ahead + scheduling slack.
        const double pipeline = per_[i].pipelineMs > 0 ? per_[i].pipelineMs : stats[i].bufferHns / 1e4;
        in.minAgeMs[i] = pipeline + captureDelayMs_ + 10.0 + 2.0 + 3.0;
        in.offsetMs[i] = offsetMs_[i];
    }
    in.safetyMs = SafetyMs(cfg_.buffer);
    in.adaptiveMs = adaptiveMs_;
    const LatencyTargets targets = ComputeLatencyTargets(in);
    t.baseMs = targets.baseMs;
    t.adaptiveMs = adaptiveMs_;

    for (int i = 0; i < 2; ++i) {
        OutputTelemetry& o = t.out[i];
        WasapiOutput& out = *outputs_[i];
        PerOutput& p = per_[i];
        o.state = states[i];
        o.rendered = in.rendered[i];
        o.direct = in.direct[i];
        o.stats = stats[i];
        o.targetMs = targets.targetMs[i];
        o.extraDelayMs = targets.extraDelayMs[i];
        o.peak = std::max(o.peak, out.TakePeak());
        o.maxCallbackGapMs = out.TakeMaxCallbackGapMs();
        if (!o.rendered || !haveAnchor) continue;

        const uint32_t gen = out.Generation();
        if (gen != p.generation) {
            p = PerOutput{};
            p.generation = gen;
            p.settleUntil = now + stats[i].bufferHns + kSettleExtraHns;
            p.lastUnderruns = stats[i].underrunEvents;
            p.lastUpdate = now;
        }

        RenderTiming rt;
        if (!out.Timing(rt) || rt.streamGeneration != gen || now - rt.hns > kStaleTimingHns) continue;
        const double ageMs = static_cast<double>(rt.hns - MasterToHns(anchor, rt.masterPos)) / kHnsPerMs;
        o.timingValid = true;
        o.ageMs = ageMs;
        // Pipeline depth. Skipped right after a realignment jump: the read position moves
        // instantly while the device still plays pre-jump audio, which would inflate it.
        const double pipeMs = (rt.readPos - rt.masterPos) / rate * 1000.0;
        if (pipeMs > 0 && pipeMs < 1000 && now >= p.jumpSettleUntil)
            p.pipelineMs = std::max(pipeMs, p.pipelineMs - 0.02); // hold peaks, decay 0.4 ms/s
        p.deviceDrift.AddSample(rt.hns, rt.devicePosFrames);
        double devPpm = 0;
        const bool devValid = stats[i].formatValid && p.deviceDrift.Ppm(stats[i].format.sampleRate, devPpm, 20.0);
        if (devValid && t.captureDriftValid) {
            o.driftValid = true;
            o.driftPpm = devPpm - capPpm;
        }
        // Feedforward: consume master frames at the rate the source currently produces them.
        // Low-passed (tau ~2 s) and bounded so estimator transients (e.g. playback stopping)
        // cannot cause sudden rate changes.
        const double ffRaw = (devValid && capShortValid) ? std::clamp((capShortPpm - devPpm) * 1e-6, -1500e-6, 1500e-6) : 0.0;
        p.feedforward += 0.025 * (ffRaw - p.feedforward);
        const double feedforward = p.feedforward;

        // Underruns mean the target delay is too tight for this system: widen it a little.
        if (stats[i].underrunEvents > p.lastUnderruns) {
            if (now > p.settleUntil) {
                adaptiveMs_ = std::min(kMaxAdaptiveMs, adaptiveMs_ + 10.0);
                LOG_WARN(LogChannel::Sync, L"Output %d underrun (%llu total); safety margin raised to %.0f ms", i + 1,
                         stats[i].underrunEvents, adaptiveMs_);
            }
            p.lastUnderruns = stats[i].underrunEvents;
            lastUnderrunHns_ = now;
        }

        if (now < p.settleUntil) {
            o.errorMs = ageMs - o.targetMs;
            continue;
        }
        const double dt = std::clamp((now - p.lastUpdate) / 1e7, 0.0, 0.5);
        p.lastUpdate = now;
        const DriftController::Action a =
            p.controller.Update((ageMs - o.targetMs) / 1000.0, dt, syncOn, tuning, feedforward);
        if (a.jump) {
            double frames = a.jumpSeconds * rate;
            if (!syncOn) frames = std::round(frames);
            out.SetRateCorrection(a.correction); // keep tracking the learned clock difference
            out.RequestJump(frames);
            p.settleUntil = now + stats[i].bufferHns + kSettleExtraHns;
            p.jumpSettleUntil = p.settleUntil;
            LOG_SYNC(L"Output %d realigned by %+.1f ms (delay %.1f ms, target %.1f ms)", i + 1,
                     a.jumpSeconds * 1000.0, ageMs, o.targetMs);
            o.errorMs = ageMs - o.targetMs;
        } else {
            out.SetRateCorrection(a.correction);
            o.errorMs = p.controller.FilteredErrorMs();
        }
        o.correctionPpm = out.RateCorrection() * 1e6;
        o.locked = p.controller.LockedSeconds();
    }

    // Slowly give back adaptive margin after a quiet period.
    if (adaptiveMs_ > 0 && now - lastUnderrunHns_ > 30 * kHnsPerSecond)
        adaptiveMs_ = std::max(0.0, adaptiveMs_ - 0.25 * kSyncPeriodMs / 1000.0);

    const int rendered = static_cast<int>(t.out[0].rendered) + static_cast<int>(t.out[1].rendered);
    const bool anyDirect = t.out[0].direct || t.out[1].direct;
    if (t.out[0].rendered && t.out[1].rendered && t.out[0].timingValid && t.out[1].timingValid) {
        t.diffValid = true;
        t.diffMs = t.out[0].errorMs - t.out[1].errorMs;
    }
    if (rendered == 0) {
        t.status = SyncStatus::Inactive;
    } else if (!syncOn) {
        t.status = SyncStatus::SyncOff;
    } else if (rendered == 1 && !anyDirect) {
        t.status = SyncStatus::SingleOutput;
    } else if (rendered == 2) {
        // "In sync" means the two outputs are aligned *with each other* — what the listener
        // hears. A shared shift of both (e.g. the source paused) does not break that.
        t.status = (t.diffValid && std::fabs(t.diffMs) <= DriftController::kLockToleranceMs)
                       ? SyncStatus::Synchronized
                       : SyncStatus::Aligning;
    } else {
        // Mirror mode: only the rendered output can be judged (against its own target).
        bool locked = true;
        for (const auto& o : t.out)
            if (o.rendered && o.locked < 2.0) locked = false;
        t.status = locked ? SyncStatus::Synchronized : SyncStatus::Aligning;
    }

    if (now - lastStatusLog_ > 30 * kHnsPerSecond) {
        lastStatusLog_ = now;
        for (int i = 0; i < 2; ++i) {
            const auto& o = t.out[i];
            if (!o.rendered) continue;
            LOG_SYNC(L"Output %d: delay %.1f ms target %.1f ms err %+.2f ms corr %+.0f ppm drift %s underruns %llu",
                     i + 1, o.ageMs, o.targetMs, o.errorMs, o.correctionPpm,
                     o.driftValid ? FormatW(L"%+.1f ppm", o.driftPpm).c_str() : L"n/a", o.stats.underrunEvents);
        }
        LOG_SYNC(L"Status: %s%s", SyncStatusLabel(t.status),
                 t.diffValid ? FormatW(L", difference %+.2f ms", t.diffMs).c_str() : L"");
    }

    std::lock_guard lock(telemetryMu_);
    telemetry_ = t;
}

} // namespace da
