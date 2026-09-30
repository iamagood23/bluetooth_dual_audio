DualAudio 1.0 - play your PC's audio on two headphones at once
================================================================

What it does
  Plays the same sound on two outputs at the same time - for example two pairs of Bluetooth
  earbuds - and keeps them in sync. It uses only standard Windows audio features: no drivers,
  no system changes, no administrator rights.

Quick start
  1. Pair and connect both earbuds in Windows Settings > Bluetooth & devices
     (DualAudio has an "Bluetooth settings" link for this).
  2. Run DualAudio.exe.
  3. Choose Output 1 and Output 2.
  4. Press "Start Dual Audio".

  Best results: in Windows Settings > Sound, set the output to a third device (for example your
  laptop speakers) and mute it. Otherwise Windows plays one of the earbuds directly and
  DualAudio can only adjust the other one ("Mirror mode").

Getting the two earbuds in sync
  * Sync playback (on by default) keeps both outputs aligned and corrects clock drift.
  * Earbuds have their own internal delay that Windows cannot see. If one sounds late:
      - "Auto calibrate": place both earbuds next to your PC's microphone and press it
        (about 10 seconds, test tones, nothing is recorded), or
      - "Fine-tune delay": add delay to the earlier output until they match.

If a device disconnects
  DualAudio keeps playing on the other output and resumes automatically when Windows
  reconnects the device. You never need to restart DualAudio or Windows.

Settings (gear icon)
  Theme (System default / Light / Dark), start minimized, start playing automatically,
  open at Windows startup, audio source (all apps or one app), buffer size, sample rate,
  sync strength, clipping protection, microphone calibration, diagnostics and logging.

Files
  config\settings.ini   your settings
  logs\                 app, audio, device and synchronization logs
  tools\DualAudioCli.exe  command-line test tool (optional)
  docs\                 architecture, synchronization and limitation notes
  To uninstall, delete this folder (and turn off "Open at Windows startup" first if you used it).

Requirements
  Windows 11 (or Windows 10 build 20348+), x64. No other software needed.

Limitations
  Two Bluetooth devices can't be made sample-perfectly identical; DualAudio minimizes and
  continuously corrects the difference. See docs\LIMITATIONS.md.
