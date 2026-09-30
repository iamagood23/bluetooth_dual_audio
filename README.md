# DualAudio

Play the same Windows audio on **two outputs at once** — e.g. two pairs of Bluetooth earbuds —
with continuous clock-drift correction and latency alignment. Built only on documented,
user-mode Windows Core Audio APIs: **no drivers, no hooks, no registry writes, no admin rights.**

```
Windows apps ─► process loopback (all apps except DualAudio) ─► 32-bit float timeline
                                                          ├─► resampler A ─► Earbuds A (WASAPI)
                                                          └─► resampler B ─► Earbuds B (WASAPI)
                     sync controller: IAudioClock timing · drift estimation · PI + feedforward
```

## Use it
1. Connect both headphones in **Windows Settings › Bluetooth & devices**.
2. Run `DualAudio.exe`, pick **Output 1** and **Output 2**, press **Start Dual Audio**.
3. For the tightest sync set Windows' own output to a third device (e.g. speakers) and mute
   it; otherwise Windows plays one output directly (Mirror mode).
4. If one pair sounds late: **Auto calibrate** (microphone) or **Fine-tune delay**.

Settings: theme (**System default / Light / Dark**), start minimized, start playing
automatically, open at Windows startup, audio source (all apps / one app), buffer size,
internal sample rate, sync strength, clipping protection, microphone calibration, diagnostics
panel, detailed logging, diagnostic report export.

## Build
Requirements: Windows 10/11 x64, Visual Studio 2022 or **Build Tools 2022** with *Desktop
development with C++* (MSVC v143, Windows 10/11 SDK 10.0.20348 or newer). No other dependencies.

```bat
build.bat          :: Release  -> bin\Release\DualAudio.exe
build.bat debug    :: Debug    -> bin\Debug\DualAudio.exe
build.bat all      :: both
test.bat           :: build + run the unit tests
run.bat            :: build if needed, then launch
package.bat        :: build, test, then dist\DualAudio\ + dist\DualAudio-1.0.0-win-x64.zip
```
Or open `DualAudio.sln` in Visual Studio. The release build statically links the C runtime,
so end users need nothing but the `.exe` (no VC++ redistributable, no Visual Studio).

## Repository layout
```
src/
  audio/            AudioEngine, AudioCapture (timeline), WasapiOutput, AudioRingBuffer,
                    AudioResampler, GainStage, SampleFormat, AudioClock
  devices/          DeviceEnumerator, DeviceMonitor, AudioSessions, DeviceState
  synchronization/  ClockDriftEstimator, LatencyCompensator, SyncController
  calibration/      AcousticCalibrator
  diagnostics/      Logger
  application/      ApplicationController, Settings
  ui/               MainWindow, Pages, Gui, Canvas, Theme
  resources/        manifest (asInvoker, PerMonitorV2), icon, version info
tests/              unit tests (DualAudioTests)
tools/cli/          DualAudioCli: headless hardware/long-run test harness
tools/*.ps1         icon generator, UI capture/click helpers for UI testing
docs/               ARCHITECTURE, AUDIO_PIPELINE, SYNCHRONIZATION, LIMITATIONS, TESTING, SYSTEM_IMPACT
```

## Hardware test harness
```bat
bin\Release\DualAudioCli.exe list
bin\Release\DualAudioCli.exe run --a 0 --b 1 --seconds 1800 --volume 0 --csv run.csv
```
`--volume 0` exercises the full engine silently; `--force-render` renders even to the Windows
default device (use with `--volume 0`). See `docs/TESTING.md`.

## System impact
Bluetooth drivers modified: **NO** · Audio drivers modified: **NO** · Kernel drivers installed:
**NO** · Windows audio services modified: **NO** · Registry modifications: **NONE** ·
Administrator privileges required: **NO** · Virtual audio driver required: **NO**.
Details: `docs/SYSTEM_IMPACT.md`.
