# Testing

## 1. Automated unit tests (`test.bat`)
No audio hardware needed. 25 tests covering:

| Area | What is verified |
|---|---|
| Ring buffer | timeline-indexed writes/reads, wrap-around, guard mirror contiguity, safe window |
| Resampler | same-rate transparency (< 1e-3), unity DC gain at all phases, 48→44.1 kHz passband (1 kHz, 18 kHz) and **alias rejection (23 kHz: −93.8 dB)**, smooth ±500 ppm rate change |
| Gain / limiter | bit-transparent at unity, limiter keeps output ≤ 1.0 and counts overs, 10 ms volume ramp |
| Sample formats | WAVEFORMATEXTENSIBLE parsing, int16 round trip + clamp, packed 24-bit, mono downmix, 5.1 mapping |
| Drift estimation | ±1.5 ppm accuracy on a jittery +37 ppm clock; reset on discontinuity |
| Latency targets | slowest-output base, relative offsets, Mirror-mode constraints |
| Sync loop (closed-loop simulation) | drift correction at all strengths, ±755/±1500 ppm source offsets without jumps, single jump for large steps, learned drift kept across jumps, feedforward on a ±1000 ppm wandering source (max error 9.6 → 2.0 ms), sync-off behaviour, noise robustness |
| Settings | INI round trip incl. Unicode, missing file → defaults |

## 2. Hardware harness (`tools\DualAudioCli.exe`)
Runs the real engine against real endpoints. `--volume 0` makes it silent, so it can run next to
normal use; `--csv` records one row per second; `tools\analyze-run.ps1` summarizes a CSV.

## 3. Results on the development machine
Windows 11 Home build 26200, AMD Ryzen laptop, Realtek HD Audio, MediaTek Bluetooth adapter.
Devices: OnePlus Buds 3 (BT A2DP), CMF Buds (BT A2DP), Realtek speakers (wired/built-in),
Steam Streaming Speakers / Microphone (virtual). "Difference" = residual misalignment of the
Windows-measurable path (up to the driver), after the first 10 s.

| Test | Result |
|---|---|
| Endpoint enumeration & classification | Bluetooth / Wired / Virtual correctly identified via IDeviceTopology; hot-plug updates the list live (observed when the Buds were switched off and on) |
| Process loopback, all apps | works; 48 kHz float, 10 ms packets, valid QPC timestamps |
| Process loopback, one app (include mode) | captures only the target process (tone at −30 dBFS measured 0.0317 peak) |
| Capture while the default endpoint is **muted** | unaffected (0.0316 peak) → "speakers muted as Windows default" setup works |
| **Bluetooth + Bluetooth** (CMF Buds + OnePlus Buds 3), 120 s | synchronized **100 %**; mean diff **0.003 ms**, p99 0.007 ms, max 0.008 ms; 0 underruns |
| Bluetooth + wired (OnePlus + Realtek), 60 s | synchronized after 4 s; mean diff 0.10 ms, max 0.30 ms; speaker drift +11 ppm corrected; 0 underruns |
| Virtual + virtual, 12 cold starts | 0 start-up underruns (after the timeline pre-roll fix) |
| GUI: select outputs, start, Dual mode | both outputs Playing, formats shown from endpoints, synchronized, difference 0.23 ms |
| GUI: Windows default = one output | Mirror mode detected, that output shown "Played by Windows", banner with Sound-settings link |
| GUI: theme System / Light / Dark | title bar and all controls switch instantly; setting persisted |
| Clean shutdown | log shows streams/threads/COM released and "exited cleanly" every time |
| Long run: see §4 | |

## 4. Long-duration runs
30-minute runs, OnePlus Buds 3 (Bluetooth) + Realtek speakers (wired), music playing on the PC
the whole time, `DualAudioCli run ... --seconds 1800 --volume 0 --csv`, summarized with
`tools\analyze-run.ps1` (first 10 s excluded).

| Metric | First design (7.4 min before fixes) | 2nd iteration (30 min) | **Final (30 min)** |
|---|---|---|---|
| Time synchronized (≤ 2 ms between outputs) | 77.7 % | 88.0 % | **97.0 %** |
| Inter-output difference median / p99 / max | 0.10 / 3.9 / 5.2 ms | 0.10 / 4.0 / 14.2 ms | **0.12 / 3.4 / 8.8 ms** |
| Underrun events (A / B) | 1 / 1 | 0 / 1 | **0 / 0** |
| Realignment jumps | many | 6 | **1 (start-up)** |
| Rate correction range | ±600 ppm (saturated) | −2742 … +1214 ppm | **−857 … +1352 ppm** |
| Measured source (timeline) clock | "−755 ppm" | −29 ppm | **−0.3 ppm** |
| Source gaps filled with silence | 11 | 6 | 92 (3–30 ms gaps now detected) |

What changed between iterations, and why (all found from these measurements):
1. Integral term could not exceed the fast-correction cap and was reset by jumps → a source-rate
   offset larger than 600 ppm caused periodic realignments. Now tracked up to 3000 ppm and kept.
2. The capture timeline treated timestamp jumps under 15 ms as jitter and let its time reference
   step; both outputs then "corrected" a shift that did not matter, and the drift estimator saw a
   phantom −755 ppm clock. Measurement showed loopback timestamps are exact (< 0.1 ms on 99.9 %
   of packets) with occasional real 3–30 ms source gaps, so the timeline is now phase-locked and
   fills any gap > 3 ms with silence.
3. Jump thresholds after lock lowered (Normal 20 → 10 ms), jump size taken from the median of
   recent raw errors so a step is fixed with one jump.

Remaining deviations in the final run were all on the wired Realtek output (its measured delay
stepped by +3…+8.5 ms a few times between minutes 19 and 23, with no source gap at the same
time), each corrected by rate slewing within ~5 s without underruns. The Bluetooth output stayed
within ±0.3 ms of its target for the whole run.

1-hour and 2-hour runs: not yet executed on this machine. Command:
`DualAudioCli run --a <n> --b <n> --seconds 7200 --volume 0 --csv 2h.csv` then
`powershell -File tools\analyze-run.ps1 -Csv 2h.csv`.

## 5. Manual test checklist (for release validation)
Device configurations — for each: select, start, listen 5 min, check Diagnostics:
- [x] Bluetooth + Bluetooth   - [x] Bluetooth + Wired   - [ ] Bluetooth + USB   - [ ] Bluetooth + HDMI

Device failures (DualAudio must keep the other output playing and never crash):
- [ ] A disconnects (turn earbuds off) → "Output 1 disconnected. Waiting for Windows to reconnect it…"
- [ ] A reconnects → restored and resynchronized automatically
- [ ] B disconnects / reconnects (same)
- [ ] Both disconnect → both waiting; app idle but responsive
- [x] Windows default device changes → Mirror/Dual switch live (observed during development)
- [ ] Bluetooth turned off / on in Settings
- [ ] Device format changed in Sound Control Panel → output reinitializes

Audio content: silence, speech, music, bass-heavy, high-frequency, rapid transients (castanets),
system notification sounds. Check no clicks, no pitch wobble, stereo intact (use a left/right
channel test), Diagnostics "Limiter engaged" stays 0 at normal levels.

Synchronization: initial sync (status "In sync" within ~5 s), steady-state drift (Diagnostics
clock drift stable), reconnection resync, 30 min / 1 h / 2 h runs with
`DualAudioCli run ... --seconds 3600 --csv` and `analyze-run.ps1`.

System integrity after closing: Windows Bluetooth, audio devices, default device, volumes and
other apps unchanged; `tasklist | find "DualAudio"` shows nothing.
