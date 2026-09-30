#pragma once
// Stateless variable-ratio windowed-sinc interpolator.
//
// Given a contiguous run of interleaved stereo input frames and a fractional position, returns
// the band-limited value at that position. Because it is stateless, the render thread can
// resample at a continuously varying ratio (clock-drift correction) and jump/crossfade between
// timeline positions without filter-history artifacts.
//
// Kernel: Kaiser-windowed sinc, polyphase table with linear interpolation between phases.
// Cutoff is placed below the lower of the two Nyquist frequencies, so downsampling is
// anti-aliased. All allocation happens in Configure(); Interpolate() is real-time safe.
#include <cstdint>
#include <vector>

namespace da {

class AudioResampler {
public:
    static constexpr int kMaxTaps = 256;

    // inRate: timeline rate, outRate: device rate. Not real-time safe.
    void Configure(double inRate, double outRate, int baseTaps = 96, int phases = 512,
                   double kaiserBeta = 8.6, double passband = 0.91);

    // Number of input frames needed before/after the integer position.
    // Interpolate(pos) reads frames [floor(pos) - HalfTaps() + 1, floor(pos) + HalfTaps()].
    int HalfTaps() const noexcept { return taps_ / 2; }
    int Taps() const noexcept { return taps_; }

    // window: pointer to frame floor(pos) - HalfTaps() + 1 (interleaved stereo, Taps() frames).
    // frac in [0, 1).
    void Interpolate(const float* window, double frac, float& outL, float& outR) const noexcept;

    // Exact identity case (same rates): callers may bypass when frac == 0 and no correction.
    bool SameRate() const noexcept { return sameRate_; }
    double Cutoff() const noexcept { return cutoff_; } // cycles per input sample

private:
    std::vector<float> table_; // (phases_ + 1) rows x taps_
    int taps_ = 0;
    int phases_ = 0;
    double cutoff_ = 0.5;
    bool sameRate_ = true;
};

} // namespace da
