# Architecture

DualAudio is a normal user-mode Windows application built on documented Core Audio APIs.
It installs nothing, needs no administrator rights and never touches drivers, services or
Bluetooth configuration. Windows remains responsible for pairing, profiles, codecs and
reconnection; DualAudio only handles audio after Windows exposes a device as an audio endpoint.

```
 UI thread (Win32 + Direct2D)                       ┌──────────────────────────────────────┐
   MainWindow / Pages / Gui  ── commands ──────────►│ ApplicationController (worker thread) │
        ▲   state snapshots + telemetry (lock-free) │  settings · routing · reconnection    │
        └────────────────────────────────────────── │  DeviceEnumerator · DeviceMonitor     │
                                                     └──────────────────┬───────────────────┘
                                                                        ▼
                                                     ┌──────────────────────────────────────┐
                                                     │ AudioEngine                           │
   ActivateAudioInterfaceAsync (process loopback) ─► │  AudioCapture  "master timeline" thr. │
                                                     │     │ QPC-timestamped float frames    │
                                                     │     ├─► ring A ─► WasapiOutput A thr. ─┼─► Endpoint A
                                                     │     └─► ring B ─► WasapiOutput B thr. ─┼─► Endpoint B
                                                     │  Sync thread: IAudioClock timing,     │
                                                     │  drift estimation, PI + feedforward   │
                                                     └──────────────────────────────────────┘
```

## Threads

| Thread | Priority | Work | Never does |
|---|---|---|---|
| UI | normal | draws, handles input, reads snapshots | block on audio or Bluetooth |
| Controller | normal | commands, device events, retries, settings, calibration start | real-time work |
| Capture (timeline) | MMCSS "Pro Audio" | drains loopback packets, fills both rings, synthesizes silence when idle | allocate, log, lock |
| Output A / Output B | MMCSS "Pro Audio" | resample + gain + convert into the WASAPI buffer, publish IAudioClock timing | allocate, log, lock |
| Sync | normal (20 Hz) | measure delays, estimate drift, steer outputs, publish telemetry, log | touch audio buffers |
| Logger | normal | writes `logs\*.log` | — |
| Calibration (on demand) | normal + mic capture thread | chirp analysis | — |

Real-time threads communicate only through atomics, a sequence lock (`common/SeqLock.h`) and
single-producer ring buffers. All allocations happen before a stream starts.

## Modules (`src/`)

| Folder | Contents |
|---|---|
| `audio/` | `AudioEngine`, `AudioCapture` (timeline), `WasapiOutput`, `AudioRingBuffer`, `AudioResampler`, `GainStage`, `SampleFormat`, `AudioClock` (timing records) |
| `devices/` | `DeviceEnumerator` (IMMDeviceEnumerator + IDeviceTopology classification), `DeviceMonitor` (IMMNotificationClient), `AudioSessions` (IAudioSessionManager2 for the one-app source) |
| `synchronization/` | `ClockDriftEstimator`, `LatencyCompensator`, `SyncController` (DriftController) |
| `calibration/` | `AcousticCalibrator` (optional microphone chirp measurement) |
| `diagnostics/` | `Logger` (async, per-channel files) |
| `application/` | `ApplicationController`, `Settings` (INI), Startup-folder shortcut |
| `ui/` | `MainWindow`, `Pages` (Main / Settings / Diagnostics), `Gui` (immediate-mode widgets), `Canvas` (Direct2D/DirectWrite), `Theme` |

## Why these technologies

* **C++20 + WASAPI** directly: lowest-level documented audio path, exact control of timing.
* **Custom Direct2D UI** instead of WinUI 3: no Windows App SDK runtime to deploy, a single
  self-contained `.exe` (static CRT), and full control of Light/Dark/System theming. The
  audio engine never depends on the UI.
* **Process loopback** (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, Windows 10 build
  20348+ / all Windows 11) captures system audio *without a virtual driver*, excluding
  DualAudio's own output so there is no feedback loop.

## Operating modes

| Mode | When | Behaviour |
|---|---|---|
| **Dual** | Neither output is the Windows default device | DualAudio renders both outputs and aligns both. Best sync. |
| **Mirror** | One output *is* the Windows default | Windows already plays that output; rendering it too would double the audio, so DualAudio feeds only the other output (which can be delayed but not advanced). |
| **Single** | Only one output selected/available | Plays to one output. |
| Fallback capture | Process loopback unavailable | Classic loopback of the default endpoint (still documented WASAPI, still driver-free). |
| Unsupported | Neither capture method works | Stops and says: *"Dual-output audio cannot be provided using the current driver-free configuration. No system drivers were modified."* |

Tip shown in Mirror mode: set Windows' output to another device (e.g. laptop speakers) and
mute it. Verified on real hardware: process loopback still captures everything while that
endpoint is muted, and DualAudio then runs in Dual mode.

## Failure handling

* Every WASAPI call's `HRESULT` is checked. `AUDCLNT_E_DEVICE_INVALIDATED`,
  `AUDCLNT_E_SERVICE_NOT_RUNNING`, `AUDCLNT_E_RESOURCES_INVALIDATED` end the stream;
  `AUDCLNT_E_BUFFER_ERROR` / `AUDCLNT_E_BUFFER_SIZE_ERROR` are treated as transient (skip a
  cycle, give up after 50 in a row); `AUDCLNT_E_UNSUPPORTED_FORMAT` marks the output failed.
* `IAudioSessionEvents::OnSessionDisconnected` distinguishes device removal from a format
  change (format change → immediate re-initialization with the new mix format).
* A render thread that receives no buffer events for 4 s is treated as stalled.
* On loss: stop that endpoint, release its COM objects, re-enumerate. If Windows still lists the
  device as active, retry after 1 s, 3 s, 8 s; otherwise wait for Windows' own device
  notification (DualAudio never polls or "hammers" the Bluetooth stack). When the endpoint
  returns: reinitialize → restore → resynchronize. The other output keeps playing throughout.
* Exceptions in audio threads or controller commands are caught at the thread boundary; one
  failing output can never take down the UI or the other output.
* On exit: streams stopped, threads joined, COM objects released, handles closed, settings saved.
