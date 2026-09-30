#pragma once
// Per-output closed-loop alignment.
//
// Input: timing error e = (measured capture-to-playback delay) - (target delay).
// Output: either a small resampling-rate correction (PI control, parts-per-million, far below
// audible pitch change) or, for large errors (start-up, reconnection, offset changes), a single
// crossfaded jump of the read position. Never pauses the device.
namespace da {

enum class SyncStrength { Gentle = 0, Normal, Strong };

struct SyncTuning {
    double kp;             // 1/s
    double ki;             // 1/s^2
    double maxCorrection;  // dimensionless (600e-6 = 600 ppm ~ 1 cent)
    double jumpThresholdMs;
    double emaAlpha;       // measurement smoothing per update
};

SyncTuning TuningFor(SyncStrength s);

class DriftController {
public:
    struct Action {
        bool jump = false;
        double jumpSeconds = 0.0; // positive: skip ahead (reduce delay)
        double correction = 0.0;  // playback speed = nominal * (1 + correction)
    };

    void Reset();
    // errorSec: measured - target. dtSec: time since last update. feedforward: the measured
    // rate difference between the audio source and this device (dimensionless), applied
    // directly so the loop only has to trim the remainder (0 if unknown).
    Action Update(double errorSec, double dtSec, bool syncEnabled, const SyncTuning& tuning,
                  double feedforward = 0.0);
    double FilteredErrorMs() const { return filtered_ * 1000.0; }
    bool HasEstimate() const { return initialized_; }
    // Seconds the filtered error has stayed within +/-lockToleranceMs.
    double LockedSeconds() const { return lockedFor_; }

    static constexpr double kLockToleranceMs = 2.0;
    static constexpr double kSyncOffSafetyMs = 250.0;
    static constexpr double kAcquisitionThresholdMs = 4.0;
    // Largest steady clock-rate difference that is tracked (3000 ppm). Real endpoints measured
    // during development ranged from ~0 to ~760 ppm relative to the process-loopback source.
    static constexpr double kMaxTracking = 3000e-6;
    static constexpr int kMaxAcquisitionJumps = 2;

private:
    void ResetLoop();

    double filtered_ = 0.0;
    double integral_ = 0.0;
    double lockedFor_ = 0.0;
    bool initialized_ = false;
    bool acquired_ = false;
    int acquisitionJumps_ = 0;
    double raw_[3] = {0, 0, 0};
    unsigned rawCount_ = 0;
};

} // namespace da
