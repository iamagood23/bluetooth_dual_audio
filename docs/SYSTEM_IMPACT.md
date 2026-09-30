# Driver / system-impact report

| Question | Answer |
|---|---|
| Bluetooth drivers modified | **NO** |
| Audio drivers modified | **NO** |
| Kernel drivers installed | **NO** |
| Virtual audio driver required / installed | **NO** |
| Windows audio services modified, stopped or restarted | **NO** |
| Registry modifications | **NONE** (one read-only query: `HKCU\…\Themes\Personalize\AppsUseLightTheme`, to follow the Windows light/dark setting) |
| Administrator privileges required | **NO** (`requestedExecutionLevel = asInvoker`) |
| Injection into `audiodg.exe` or other processes, hooks, patches | **NO** |
| Bluetooth pairing, profiles, codecs, firmware | untouched — handled entirely by Windows |
| Endpoint formats / default device / volumes | never changed (streams use `AUDCLNT_STREAMFLAGS_NOPERSIST`, so DualAudio's own session volume is not persisted either) |

## Files DualAudio writes
* `config\settings.ini` and `logs\*.log` next to `DualAudio.exe`; if that folder is not
  writable (e.g. Program Files), `%LOCALAPPDATA%\DualAudio\` instead.
* Only if the user enables *Open at Windows startup*: `DualAudio.lnk` in the user's Startup
  folder (`shell:startup`). Turning the option off deletes it.
* Diagnostic reports, only when the user clicks *Export*.

Nothing else. Uninstalling = deleting the folder (and the Startup shortcut, if enabled).

## APIs used (all documented, user mode)
`CoInitializeEx`, `IMMDeviceEnumerator`, `IMMDevice`, `IMMNotificationClient`,
`IDeviceTopology`/`IConnector` (to tell Bluetooth/USB/HDMI/wired apart), `IAudioClient`,
`IAudioRenderClient`, `IAudioCaptureClient`, `IAudioClock`, `IAudioSessionControl`/
`IAudioSessionEvents`, `IAudioSessionManager2`, `ActivateAudioInterfaceAsync` with
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, `AvSetMmThreadCharacteristicsW` (MMCSS, per
thread), `DwmSetWindowAttribute` (title-bar colour), Direct2D, DirectWrite, `IShellLinkW`,
`ShellExecuteW("ms-settings:bluetooth" / "ms-settings:sound")`.

## After closing DualAudio
Windows Bluetooth, audio drivers, audio services, device formats, default devices and other
applications are exactly as they were. All streams are stopped, threads joined, COM interfaces
released and handles closed before the process exits (logged as
`Engine stopped; all streams, threads and COM objects released` / `DualAudio exited cleanly`).
