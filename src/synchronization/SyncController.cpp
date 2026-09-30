#include "synchronization/SyncController.h"

#include <algorithm>
#include <cmath>

namespace da {

SyncTuning TuningFor(SyncStrength s) {
    // Plant: de/dt = -correction. PI gives e'' + kp e' + ki e = 0:
    //   Gentle  wn 0.063 rad/s, zeta 0.79     Normal  wn 0.141, zeta 0.88
    //   Strong  wn 0.245, zeta 1.02
    // All are well damped and far slower than the ~0.3 s measurement delay, so the loop is
    // stable. maxCorrection limits the fast (proportional) part; see kMaxTracking.
    switch (s) {
    case SyncStrength::Gentle: return {0.10, 0.004, 300e-6, 25.0, 0.10};
    case SyncStrength::Strong: return {0.50, 0.060, 1200e-6, 5.0, 0.20};
    default: return {0.25, 0.020, 600e-6, 10.0, 0.15};
    }
}

void DriftController::Reset() {
    ResetLoop();
    integral_ = 0.0;
    acquisitionJumps_ = 0;
    acquired_ = false;
}

void DriftController::ResetLoop() {
    // Keeps integral_: it holds the learned clock-rate difference, which a jump does not change.
    filtered_ = 0.0;
    lockedFor_ = 0.0;
    initialized_ = false;
    rawCount_ = 0;
}

DriftController::Action DriftController::Update(double e, double dt, bool syncEnabled, const SyncTuning& t,
                                                double feedforward) {
    const double ff = syncEnabled ? std::clamp(feedforward, -kMaxTracking, kMaxTracking) : 0.0;
    Action a;
    if (!std::isfinite(e)) return a;
    raw_[rawCount_++ % 3] = e;
    if (!initialized_) {
        filtered_ = e;
        initialized_ = true;
    } else {
        filtered_ += t.emaAlpha * (e - filtered_);
    }

    // Until the first lock, a few-ms error is fixed with one crossfaded jump rather than
    // tens of seconds of slewing. Limited so a noisy device cannot cause repeated jumps.
    double thresholdMs = syncEnabled ? t.jumpThresholdMs : kSyncOffSafetyMs;
    if (syncEnabled && !acquired_ && acquisitionJumps_ < kMaxAcquisitionJumps)
        thresholdMs = std::min(thresholdMs, kAcquisitionThresholdMs);
    // Jump only when both the smoothed error and the median of the last three raw measurements
    // exceed the threshold (two consistent samples: one bad reading cannot trigger it), and jump
    // by that median: after a step change the smoothed value still lags the real error.
    double median = e;
    if (rawCount_ >= 3) {
        double m[3] = {raw_[0], raw_[1], raw_[2]};
        std::sort(m, m + 3);
        median = m[1];
    }
    if (std::fabs(filtered_) * 1000.0 > thresholdMs && std::fabs(median) * 1000.0 > thresholdMs) {
        a.jump = true;
        a.jumpSeconds = median;
        if (!acquired_) ++acquisitionJumps_;
        ResetLoop();
        a.correction = syncEnabled ? ff + std::clamp(t.ki * integral_, -kMaxTracking, kMaxTracking) : 0.0;
        return a;
    }

    if (std::fabs(filtered_) * 1000.0 <= kLockToleranceMs)
        lockedFor_ += dt;
    else
        lockedFor_ = 0.0;
    if (lockedFor_ >= 1.0) acquired_ = true;

    if (!syncEnabled) {
        integral_ = 0.0;
        return a; // correction 0: plain duplication, safety jumps only
    }
    // Fast part: proportional, capped so transient corrections stay far below audibility.
    const double p = std::clamp(t.kp * filtered_, -t.maxCorrection, t.maxCorrection);
    // Slow part: integral, learns the steady clock-rate difference between the audio source
    // and this device. It may exceed maxCorrection because it only *follows the source's own
    // clock* (the source is produced at that rate), rather than bending the audio.
    // Always learning (a large source offset saturates the fast part, and that is exactly when
    // the slow part must learn); windup is bounded by the hard tracking limit, and errors large
    // enough to overshoot are handled by a jump instead.
    const double iMax = kMaxTracking / t.ki;
    integral_ = std::clamp(integral_ + filtered_ * dt, -iMax, iMax);
    const double i = std::clamp(t.ki * integral_, -kMaxTracking, kMaxTracking);
    a.correction = std::clamp(ff + p + i, -2 * kMaxTracking, 2 * kMaxTracking);
    return a;
}

} // namespace da
