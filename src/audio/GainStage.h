#pragma once
// Per-output volume, mute and clipping protection.
//
// At unity gain with no overs this stage is bit-transparent. The limiter only engages when a
// sample would exceed full scale (instant attack, ~60 ms release); it never compresses audio
// that is already inside range.
#include <cstdint>

namespace da {

class GainStage {
public:
    void Configure(double sampleRate) noexcept;

    // Real-time safe. frames: interleaved stereo in/out.
    void Process(float* stereo, uint32_t frames, float targetGain, bool clipProtection) noexcept;

    uint64_t OversDetected() const noexcept { return overs_; }
    uint64_t LimiterEngagedFrames() const noexcept { return limited_; }
    float CurrentGain() const noexcept { return gain_; }

    // Perceptual volume curve (0..1 slider) -> linear gain (square-law taper).
    static float SliderToGain(float slider) noexcept;

private:
    float gain_ = 1.0f;       // smoothed volume gain
    float reduction_ = 1.0f;  // limiter gain reduction
    float rampStep_ = 0.001f; // max gain change per frame
    float release_ = 0.999f;
    uint64_t overs_ = 0;
    uint64_t limited_ = 0;
};

} // namespace da
