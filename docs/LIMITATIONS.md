# Known limitations

1. **No perfect synchronization.** Each Bluetooth device has its own clock, codec and internal
   buffer. DualAudio aligns everything Windows can measure (typically to < 1 ms) and corrects
   drift continuously, but the delay *inside* each pair of earbuds is invisible to Windows. Use
   Auto calibrate (microphone) or Fine-tune delay to match what you hear. Some earbuds change
   their internal buffering on their own (e.g. after interference); re-calibrate if they drift
   apart audibly.
2. **The Windows default device also plays the audio.** Windows keeps sending app audio to its
   default output. If one of your chosen outputs is the default, DualAudio uses Mirror mode
   (does not render it again) and can only delay the *other* output. For full two-way alignment,
   set Windows' output to a third device (e.g. laptop speakers) and mute it — DualAudio still
   captures everything (verified).
3. **Windows switches the default automatically** when new Bluetooth headphones connect, which
   usually puts you in Mirror mode. DualAudio follows such changes live.
4. **Process loopback needs Windows 10 build 20348+** (all Windows 11 builds). On older builds
   DualAudio falls back to loopback of the default endpoint, which cannot exclude other audio and
   may be affected by that endpoint's mute.
5. **The source can have gaps.** Measured: process-loopback timestamps are exact, but the captured
   stream occasionally skips 3–30 ms (the playing app's own stream glitches, track changes). DualAudio
   fills such gaps with silence so both outputs stay aligned; the gap itself is inaudible or already
   present in the source. Clock-rate differences up to 3000 ppm are tracked (feedforward + integral).
6. **Hands-free (HFP) endpoints** ("Headset … Hands-Free") are call-quality mono. They are listed
   last and labelled "call quality". Auto-calibration never opens a Bluetooth microphone because
   that would force earbuds into hands-free mode.
7. **Apps that render directly to a non-default device** are also captured by "All apps" and would
   be heard twice on that device. Use "One app" as the source in that case.
8. **Latency.** Two-way sync means the faster output is delayed to match the slower one. Expect
   roughly 80–250 ms end to end depending on devices and buffer setting — fine for music and
   video (players compensate via the default device), not for gaming or live monitoring.
9. **Exclusive-mode apps** bypass the Windows mixer and cannot be captured.
10. **DRM-protected audio** that Windows excludes from loopback capture cannot be duplicated.
11. **Custom UI accessibility.** Full keyboard navigation and focus indicators are implemented;
    screen-reader (UI Automation) support is not yet implemented.
12. **Microphone privacy.** Auto-calibration needs "Let desktop apps access your microphone" to be
    on. If it is off, calibration reports that the microphone could not be opened.
