#include "synchronization/ClockDriftEstimator.h"

#include <cmath>

namespace da {

void ClockDriftEstimator::Reset() noexcept {
    head_ = 0;
    count_ = 0;
}

void ClockDriftEstimator::AddSample(int64_t hns, double position) noexcept {
    if (count_ > 0) {
        const int last = (head_ - 1 + kMax) % kMax;
        const int64_t dt = hns - t_[last];
        if (dt < kMinSpacing) return;
        // Position went backwards or jumped absurdly (stream restart): start over.
        const double rate = (position - p_[last]) / (dt / 1e7);
        if (position < p_[last] || !std::isfinite(rate)) {
            Reset();
        }
    }
    t_[head_] = hns;
    p_[head_] = position;
    head_ = (head_ + 1) % kMax;
    if (count_ < window_) ++count_;
}

double ClockDriftEstimator::SpanSeconds() const noexcept {
    if (count_ < 2) return 0.0;
    const int first = (head_ - count_ + kMax) % kMax;
    const int last = (head_ - 1 + kMax) % kMax;
    return (t_[last] - t_[first]) / 1e7;
}

bool ClockDriftEstimator::Ppm(double nominalRate, double& ppm, double minSpanSeconds) const noexcept {
    if (count_ < 8 || SpanSeconds() < minSpanSeconds || nominalRate <= 0) return false;
    const int first = (head_ - count_ + kMax) % kMax;
    const int64_t t0 = t_[first];
    const double p0 = p_[first];
    double sx = 0, sy = 0;
    for (int i = 0; i < count_; ++i) {
        const int k = (first + i) % kMax;
        sx += (t_[k] - t0) / 1e7;
        sy += p_[k] - p0;
    }
    const double mx = sx / count_, my = sy / count_;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < count_; ++i) {
        const int k = (first + i) % kMax;
        const double x = (t_[k] - t0) / 1e7 - mx;
        const double y = (p_[k] - p0) - my;
        sxx += x * x;
        sxy += x * y;
    }
    if (sxx <= 0) return false;
    const double slope = sxy / sxx;
    ppm = (slope / nominalRate - 1.0) * 1e6;
    // Anything beyond +/-2000 ppm is a measurement artifact, not a crystal.
    return std::isfinite(ppm) && std::fabs(ppm) < 2000.0;
}

} // namespace da
