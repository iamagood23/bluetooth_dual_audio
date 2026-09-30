// DualAudioCli - headless harness for the audio engine (hardware validation & long-run tests).
//
//   DualAudioCli list
//   DualAudioCli run --a <n> --b <n> [--seconds 60] [--volume 0] [--buffer low|balanced|stable]
//                    [--strength gentle|normal|strong] [--nosync] [--rate 44100|48000]
//                    [--source all|endpoint|pid:<n>] [--offset-a ms] [--offset-b ms] [--force-render]
//                    [--csv file]
//
// Devices are chosen by index from `list`. By default the Windows default output is treated as
// "played directly by Windows" (Mirror mode), exactly like the app; --force-render renders to it
// anyway (use with --volume 0 to avoid doubled audio).
#include "audio/AudioEngine.h"
#include "common/Clock.h"
#include "common/ComUtil.h"
#include "common/Paths.h"
#include "devices/DeviceEnumerator.h"
#include "diagnostics/Logger.h"

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace da;

namespace {

std::map<std::wstring, std::wstring> ParseArgs(int argc, wchar_t** argv, int start) {
    std::map<std::wstring, std::wstring> m;
    for (int i = start; i < argc; ++i) {
        std::wstring k = argv[i];
        if (k.rfind(L"--", 0) != 0) continue;
        k = k.substr(2);
        if (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0)
            m[k] = argv[++i];
        else
            m[k] = L"1";
    }
    return m;
}

const wchar_t* StateName(OutputState s) {
    switch (s) {
    case OutputState::Starting: return L"starting";
    case OutputState::Running: return L"running";
    case OutputState::Lost: return L"LOST";
    case OutputState::Failed: return L"FAILED";
    default: return L"idle";
    }
}

int List(DeviceEnumerator& en) {
    HRESULT hr = S_OK;
    auto devs = en.ActiveRenderDevices(&hr);
    if (FAILED(hr)) {
        wprintf(L"Enumeration failed: %s\n", HResultToString(hr).c_str());
        return 2;
    }
    wprintf(L"Active render endpoints:\n");
    for (size_t i = 0; i < devs.size(); ++i) {
        const auto& d = devs[i];
        wprintf(L"  [%zu] %-45s %-9s %s%6u Hz %u ch%s\n", i, d.name.c_str(), DeviceKindLabel(d.kind),
                d.handsFree ? L"(hands-free) " : L"", d.sampleRate, d.channels, d.isDefault ? L"  <- Windows default" : L"");
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    ComInit com(COINIT_MULTITHREADED);
    Logger::Get().SetConsoleEcho(false);
    Logger::Get().Start(LogsDirectory(), true);
    DeviceEnumerator en;
    if (FAILED(en.Initialize())) {
        wprintf(L"Cannot create IMMDeviceEnumerator\n");
        return 2;
    }
    const std::wstring cmd = argc > 1 ? argv[1] : L"list";
    if (cmd == L"list") {
        const int r = List(en);
        Logger::Get().Stop();
        return r;
    }
    if (cmd != L"run") {
        wprintf(L"unknown command\n");
        return 2;
    }

    auto args = ParseArgs(argc, argv, 2);
    auto devs = en.ActiveRenderDevices();
    auto pick = [&](const wchar_t* key) -> const AudioDeviceInfo* {
        if (!args.count(key)) return nullptr;
        const size_t idx = static_cast<size_t>(_wtoi(args[key].c_str()));
        return idx < devs.size() ? &devs[idx] : nullptr;
    };
    const AudioDeviceInfo* sel[2] = {pick(L"a"), pick(L"b")};
    if (!sel[0] && !sel[1]) {
        wprintf(L"need --a and/or --b device indexes (see `list`)\n");
        return 2;
    }
    const int seconds = args.count(L"seconds") ? _wtoi(args[L"seconds"].c_str()) : 30;
    const float volume = args.count(L"volume") ? static_cast<float>(_wtof(args[L"volume"].c_str())) : 0.0f;
    const bool forceRender = args.count(L"force-render") > 0;

    EngineConfig cfg;
    cfg.capture.timelineRate = args.count(L"rate") ? _wtof(args[L"rate"].c_str()) : 48000.0;
    const std::wstring src = args.count(L"source") ? args[L"source"] : L"all";
    if (src == L"endpoint") {
        cfg.capture.mode = CaptureMode::Endpoint;
        cfg.capture.endpointId = en.DefaultRenderId();
    } else if (src.rfind(L"pid:", 0) == 0) {
        cfg.capture.mode = CaptureMode::SingleApp;
        cfg.capture.targetPid = static_cast<DWORD>(_wtoi(src.c_str() + 4));
    }
    const std::wstring buf = args.count(L"buffer") ? args[L"buffer"] : L"balanced";
    cfg.buffer = buf == L"low" ? BufferMode::LowLatency : buf == L"stable" ? BufferMode::Stable : BufferMode::Balanced;
    const std::wstring st = args.count(L"strength") ? args[L"strength"] : L"normal";
    cfg.strength = st == L"gentle" ? SyncStrength::Gentle : st == L"strong" ? SyncStrength::Strong : SyncStrength::Normal;
    cfg.syncEnabled = args.count(L"nosync") == 0;

    std::mutex evMu;
    std::vector<std::wstring> events;
    AudioEngine engine;
    engine.Start(cfg, [&](const EngineEvent& e) {
        std::lock_guard lock(evMu);
        events.push_back(FormatW(L"event kind=%d slot=%d hr=%s", static_cast<int>(e.kind), e.slot,
                                 HResultToString(e.hr).c_str()));
    });
    const std::wstring def = en.DefaultRenderId();
    for (int i = 0; i < 2; ++i) {
        if (!sel[i]) continue;
        engine.SetVolume(i, volume);
        const wchar_t* key = i == 0 ? L"offset-a" : L"offset-b";
        if (args.count(key)) engine.SetOffsetMs(i, _wtof(args[key].c_str()));
        const bool direct = sel[i]->id == def && !forceRender;
        engine.SetDirect(i, direct);
        wprintf(L"Output %d: %s (%s)%s\n", i + 1, sel[i]->name.c_str(), DeviceKindLabel(sel[i]->kind),
                direct ? L" - Windows default, played directly (mirror)" : L"");
        if (!direct) engine.StartOutput(i, sel[i]->id);
    }

    FILE* csv = nullptr;
    if (args.count(L"csv")) {
        _wfopen_s(&csv, args[L"csv"].c_str(), L"w");
        if (csv)
            fwprintf(csv, L"t,status,diff_ms,a_age,a_target,a_err,a_corr_ppm,a_drift_ppm,a_underruns,b_age,b_target,b_err,"
                          L"b_corr_ppm,b_drift_ppm,b_underruns,capture_packets,capture_gaps\n");
    }

    double maxAbsDiff = 0, sumAbsDiff = 0;
    int diffSamples = 0, syncedSamples = 0, measuredSamples = 0;
    const int64_t t0 = NowHns();
    for (int s = 1; s <= seconds; ++s) {
        Sleep(1000);
        {
            std::lock_guard lock(evMu);
            for (auto& e : events) wprintf(L"  %s\n", e.c_str());
            events.clear();
        }
        const EngineTelemetry t = engine.Telemetry();
        const double el = (NowHns() - t0) / 1e7;
        wprintf(L"[%5.0fs] %-12s cap:%s pk=%.3f pkts=%llu gaps=%llu idle=%.1fs dly=%.1fms", el,
                SyncStatusLabel(t.status), t.capture.state == CaptureState::Running ? L"ok" : L"--", t.capturePeak,
                t.capture.packets, t.capture.gapsFilled, t.capture.idleFrames / t.timelineRate,
                t.capture.lastPacketDelayMs);
        for (int i = 0; i < 2; ++i) {
            const auto& o = t.out[i];
            if (!sel[i]) continue;
            if (o.direct) {
                wprintf(L" | %c direct", 'A' + i);
                continue;
            }
            wprintf(L" | %c %s age=%.1f tgt=%.1f err=%+.2f corr=%+.0fppm drift=%s ur=%llu j=%llu buf=%u", 'A' + i,
                    StateName(o.state), o.ageMs, o.targetMs, o.errorMs, o.correctionPpm,
                    o.driftValid ? FormatW(L"%+.1f", o.driftPpm).c_str() : L"n/a", o.stats.underrunEvents,
                    o.stats.jumps, o.stats.bufferFrames);
        }
        if (t.diffValid) {
            wprintf(L" | diff=%+.2fms", t.diffMs);
            if (s > 10) {
                maxAbsDiff = std::max(maxAbsDiff, std::fabs(t.diffMs));
                sumAbsDiff += std::fabs(t.diffMs);
                ++diffSamples;
            }
        }
        if (s > 10 && (t.status == SyncStatus::Synchronized || t.status == SyncStatus::Aligning)) {
            ++measuredSamples;
            if (t.status == SyncStatus::Synchronized) ++syncedSamples;
        }
        wprintf(L"\n");
        if (csv) {
            const auto& a = t.out[0];
            const auto& b = t.out[1];
            fwprintf(csv, L"%.1f,%s,%.3f,%.2f,%.2f,%.3f,%.1f,%.2f,%llu,%.2f,%.2f,%.3f,%.1f,%.2f,%llu,%llu,%llu\n", el,
                     SyncStatusLabel(t.status), t.diffMs, a.ageMs, a.targetMs, a.errorMs, a.correctionPpm, a.driftPpm,
                     a.stats.underrunEvents, b.ageMs, b.targetMs, b.errorMs, b.correctionPpm, b.driftPpm,
                     b.stats.underrunEvents, t.capture.packets, t.capture.gapsFilled);
            fflush(csv);
        }
    }
    const EngineTelemetry fin = engine.Telemetry();
    engine.Stop();
    if (csv) fclose(csv);

    wprintf(L"\n==== SUMMARY (%d s) ====\n", seconds);
    for (int i = 0; i < 2; ++i) {
        if (!sel[i]) continue;
        const auto& o = fin.out[i];
        wprintf(L"Output %c: %s, format %s, buffer %u frames (%.1f ms), underrun events %llu (%llu frames), overruns %llu, "
                L"jumps %llu, transient errors %llu, overs %llu, drift %s, last error %s\n",
                'A' + i, o.direct ? L"direct (Windows)" : StateName(o.state),
                o.stats.formatValid ? o.stats.format.Describe().c_str() : L"-", o.stats.bufferFrames,
                o.stats.bufferHns / 1e4, o.stats.underrunEvents, o.stats.underrunFrames, o.stats.overrunEvents,
                o.stats.jumps, o.stats.transientErrors, o.stats.overs,
                o.driftValid ? FormatW(L"%+.2f ppm", o.driftPpm).c_str() : L"n/a",
                HResultToString(o.stats.lastError).c_str());
    }
    wprintf(L"Capture: packets %llu, gaps filled %llu, dropped frames %llu, restarts %llu, clock %s\n",
            fin.capture.packets, fin.capture.gapsFilled, fin.capture.droppedFrames, fin.capture.restarts,
            fin.captureDriftValid ? FormatW(L"%+.2f ppm vs QPC", fin.captureClockPpm).c_str() : L"n/a");
    if (diffSamples)
        wprintf(L"Inter-output difference after 10 s: mean |diff| %.3f ms, max |diff| %.3f ms (%d samples)\n",
                sumAbsDiff / diffSamples, maxAbsDiff, diffSamples);
    if (measuredSamples)
        wprintf(L"Synchronized %d of %d samples after 10 s (%.1f%%)\n", syncedSamples, measuredSamples,
                100.0 * syncedSamples / measuredSamples);
    Logger::Get().Stop();
    return 0;
}
