# Synchronization

## What is measured
Every capture packet carries a QPC timestamp, so every timeline frame `n` has a capture time
`T(n)`. The timeline is *phase-locked*: timestamp jitter (measured < 0.1 ms on 99.9 % of packets) is
smoothed, and real discontinuities in the source (> 3 ms, e.g. the player pausing between tracks)
are filled with silence, so the time reference never steps. After each buffer, each render thread calls `IAudioClock::GetPosition(&pos, &qpc)`: the
device is playing device frame `pos` at time `qpc`. The render thread knows which timeline
position it wrote into every device frame, so it publishes *"at time qpc, timeline position m is
being played"*. The sync thread computes, per output:

```
delay(i) = qpc − T(m)        capture → device playback, measured, in ms
pipeline(i) = (next read position − m) / rate   audio queued inside Windows/driver
```

## Target delays
```
minimum(i) = pipeline(i) + capture delivery delay + one capture packet + look-ahead + slack
base       = max over rendered outputs of minimum(i) + safety margin (5/15/40 ms) + adaptive margin
target(i)  = base + (offset(i) − smallest offset)
```
Offsets are the per-output *Latency offset* sliders (−500…+500 ms) or the values found by
auto-calibration. Only their difference matters, so total latency stays as low as possible. In
Mirror mode the Windows-played output cannot be delayed, so the other output is delayed only
when its offset asks for more delay.

## Control loop (per output, 20 Hz)
`error = delay − target` (smoothed). Then either:

* **Jump** (crossfaded read-position change, ~5 ms): for large errors — start-up, reconnection,
  offset changes. Before the first lock a jump is used for anything above 4 ms (at most twice);
  afterwards only above 5/10/25 ms (Strong/Normal/Gentle). A jump needs two consistent
  measurements (smoothed error *and* median of the last three raw ones), and its size is that
  median, so a step change is fixed in exactly one jump.
* **Rate correction**: `speed = nominal × (1 + c)` with
  `c = feedforward + P + I`
  * **feedforward**: the measured rate difference between the audio source (10 s regression of
    the timeline vs QPC, low-passed) and this device (2 min regression of IAudioClock vs QPC);
  * **P**: fast trim, capped at 300 / 600 / 1200 ppm (Gentle / Normal / Strong; 600 ppm ≈ 1 cent);
  * **I**: learns any remaining steady difference (≤ 3000 ppm), kept across jumps.

  Feedforward and I only *follow the source's own clock* — the source is being produced at that
  rate — so they do not bend pitch relative to what the source intended.

The device is never paused, restarted or re-negotiated to synchronize.

## Measured results (this development machine, Windows 11 build 26200)
| Configuration | Result |
|---|---|
| Bluetooth (CMF Buds) + Bluetooth (OnePlus Buds 3), 120 s | synchronized 100 % of samples after 10 s; mean \|difference\| 0.003 ms, max 0.008 ms; 0 underruns |
| Bluetooth (OnePlus Buds 3) + wired speakers (Realtek), 60 s | synchronized after 4 s; mean \|difference\| 0.10 ms, max 0.30 ms; speakers +11 ppm drift corrected |
| Virtual + virtual (Steam), 12 cold starts | 0 underruns on start; synchronized within ~3 s |
| Long run: see TESTING.md | |

"Difference" is the residual misalignment of the Windows-measurable part of the path (up to
the driver).

## What cannot be measured by Windows
Bluetooth earbuds buffer audio internally (often 100–250 ms, different per model) and Windows
does not report it. That part is compensated by:
* **Auto calibrate** (optional): with both outputs rendered by DualAudio, a 60 ms chirp is played
  on one output at a time (5 × each) and recorded with a non-Bluetooth microphone. Arrival times
  are found by normalized cross-correlation and compared with the IAudioClock playback time.
  The microphone's own latency is common to both outputs and cancels out. Results are accepted
  only if ≥ 3 of 5 chirps per output are clearly detected (correlation ≥ 0.25, peak/noise ≥ 6)
  and repetitions agree within 4 ms; otherwise the app reports *"Automatic calibration
  unavailable — use manual adjustment."* Nothing is recorded to disk.
* **Fine-tune delay** sliders (manual).

## Honest limits
Two Bluetooth devices have independent clocks and buffers. DualAudio keeps the part Windows can
see aligned to well under a millisecond and continuously corrects drift, but it cannot make two
arbitrary earbuds sample-perfectly synchronous. See LIMITATIONS.md.
