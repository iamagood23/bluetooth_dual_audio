#pragma once
// Estimates how fast a clock runs relative to the system (QPC) clock, from (time, position)
// observations, by least-squares regression over a sliding window. Result in ppm.
#include <cstdint>

namespace da {

class ClockDriftEstimator {
public:
    void Reset() noexcept;
    // Number of 0.5 s samples kept (default 240 = 2 minutes; max 240).
    void SetWindow(int samples) noexcept { window_ = samples < 8 ? 8 : (samples > kMax ? kMax : samples); }
    // hns: QPC time (100 ns). position: frames played/captured at that time.
    void AddSample(int64_t hns, double position) noexcept;
    // Rate deviation from `nominalRate` frames/s, in ppm. False until enough history (minSpan).
    bool Ppm(double nominalRate, double& ppm, double minSpanSeconds = 10.0) const noexcept;
    double SpanSeconds() const noexcept;
    int Count() const noexcept { return count_; }

private:
    static constexpr int kMax = 240;              // 2 minutes at 2 samples/s
    static constexpr int64_t kMinSpacing = 5'000'000; // 0.5 s
    int64_t t_[kMax] = {};
    double p_[kMax] = {};
    int head_ = 0;
    int count_ = 0;
    int window_ = kMax;
};

} // namespace da
