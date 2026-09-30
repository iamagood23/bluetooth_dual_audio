# Audio pipeline

```
apps ─► Windows audio engine ─► default endpoint          (Windows keeps doing this)
            │
            └─ process loopback (all apps except DualAudio, or one chosen app)
                   │  48 kHz (or 44.1 kHz) · stereo · 32-bit float, converted by Windows
                   ▼
            AudioCapture: timestamps every packet (QPC), fills gaps with silence,
            drops overlaps, synthesizes silence when nothing plays → continuous timeline
                   │  master frame index n  ↔  capture time T(n)
          ┌────────┴────────┐
          ▼                 ▼
      ring A (2.7 s)    ring B (2.7 s)      slot = n mod capacity (index *is* the timeline)
          ▼                 ▼
   windowed-sinc        windowed-sinc       variable ratio = (timeline rate / device rate)
   resampler A          resampler B           × (1 + sync correction)
          ▼                 ▼
   volume · mute ·      volume · mute ·     bit-transparent at 100 % with no overs
   clip protection      clip protection
          ▼                 ▼
   device format A      device format B     float32 / int16 (TPDF dither) / int24 / int32,
          ▼                 ▼                 mono downmix or L/R into FL/FR
   WASAPI shared        WASAPI shared       event-driven, own thread, own clock
   endpoint A           endpoint B
```

## Internal format
32-bit float, stereo, 48 kHz by default (44.1 kHz selectable). The capture is requested in this
format directly (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), so there is exactly one conversion into
the pipeline and one conversion out per device.

## Resampler
Kaiser-windowed sinc (β = 8.6), 96 taps (widened automatically when downsampling), 512
polyphase rows with linear interpolation between rows, each row normalized to unity DC gain.
It is *stateless*: it reads directly from the ring at a fractional position, so the rate can be
changed continuously (drift correction) and the position can jump with a 256-frame crossfade
without filter-history artifacts. Measured (unit tests): 23 kHz alias rejected by **93.8 dB**
when going 48 → 44.1 kHz; 1 kHz and 18 kHz pass at full level; same-rate path error < 1e-3.
If rates match and synchronization is off, samples are copied bit-exactly.

## Gain and clipping protection
Volume uses a square-law taper with a 10 ms ramp (no zipper noise). At unity gain the stage is
bit-transparent. Samples above ±1.0 are counted; with clipping protection on (default), an
instant-attack / 60 ms-release peak limiter engages *only* while a sample would exceed full scale.
No compression, EQ, normalization or other DSP is applied. Integer formats are clamped as a last
resort and counted.

## Buffers and underrun handling
* WASAPI buffer: 20 / 40 / 80 ms for Low latency / Balanced / Stable.
* Ring buffers: 2.6 s, enough for the base delay plus a 1-second offset difference.
* Underrun (capture late): the missing part of that buffer is silence and the read position
  does not advance; the sync controller then realigns and the adaptive safety margin grows by
  10 ms (up to 80 ms), decaying slowly after 30 s without underruns.
* Overrun (reader starved > ~2.5 s): the reader resynchronizes near the newest audio.
* All counters (underruns, overruns, realignments, transient errors, limiter activity, clamped
  samples) are visible in Diagnostics and in the exported report.
