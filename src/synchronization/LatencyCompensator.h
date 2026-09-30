#pragma once
// Chooses the capture-to-playback delay ("age") each output should run at so that both are
// heard together.
//
//   base      = the largest minimum delay any rendered output can sustain + safety margins
//   offset    = user/calibration latency offsets; only their *difference* matters, so the
//               smallest is normalized to zero (keeps total latency minimal)
//   target[i] = base + normalized offset[i]
//
// An output played directly by Windows (Mirror mode) cannot be delayed; the other output is
// delayed relative to it only when its offset asks for more delay.
namespace da {

struct LatencyInputs {
    bool rendered[2] = {false, false}; // DualAudio renders this output
    bool direct[2] = {false, false};   // Windows plays this output directly (default device)
    double minAgeMs[2] = {0, 0};       // smallest delay this output can run at without underruns
    double offsetMs[2] = {0, 0};       // user/calibrated offset, -500..+500
    double safetyMs = 15.0;
    double adaptiveMs = 0.0;           // extra margin learned from underruns
};

struct LatencyTargets {
    double baseMs = 0.0;
    double targetMs[2] = {0, 0};
    double extraDelayMs[2] = {0, 0}; // the normalized offset actually applied
};

LatencyTargets ComputeLatencyTargets(const LatencyInputs& in);

} // namespace da
