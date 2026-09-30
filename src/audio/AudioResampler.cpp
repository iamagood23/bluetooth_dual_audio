#include "audio/AudioResampler.h"

#include <algorithm>
#include <cmath>

namespace da {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Zeroth-order modified Bessel function of the first kind (series expansion).
double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}
} // namespace

void AudioResampler::Configure(double inRate, double outRate, int baseTaps, int phases,
                               double kaiserBeta, double passband) {
    sameRate_ = std::fabs(inRate - outRate) < 0.5;
    const double ratio = std::min(1.0, outRate / inRate); // < 1 when downsampling
    cutoff_ = 0.5 * passband * ratio;

    // Widen the kernel when downsampling so the transition band stays the same in output terms.
    int taps = static_cast<int>(std::ceil(baseTaps / ratio));
    taps = std::clamp(taps + (taps & 1), 8, kMaxTaps);
    taps_ = taps;
    phases_ = phases;

    const int half = taps_ / 2;
    const double i0Beta = BesselI0(kaiserBeta);
    table_.assign(static_cast<size_t>(phases_ + 1) * taps_, 0.0f);
    for (int p = 0; p <= phases_; ++p) {
        const double frac = static_cast<double>(p) / phases_;
        float* row = &table_[static_cast<size_t>(p) * taps_];
        double sum = 0.0;
        for (int k = 0; k < taps_; ++k) {
            // Distance from the interpolation point to input sample k of the window.
            const double x = static_cast<double>(k - half + 1) - frac;
            const double arg = 2.0 * cutoff_ * x;
            const double sinc = (std::fabs(arg) < 1e-12) ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            const double t = x / static_cast<double>(half);
            const double w = (std::fabs(t) >= 1.0) ? 0.0 : BesselI0(kaiserBeta * std::sqrt(1.0 - t * t)) / i0Beta;
            const double h = 2.0 * cutoff_ * sinc * w;
            row[k] = static_cast<float>(h);
            sum += h;
        }
        // Normalize each phase to unity DC gain.
        if (sum != 0.0)
            for (int k = 0; k < taps_; ++k) row[k] = static_cast<float>(row[k] / sum);
    }
}

void AudioResampler::Interpolate(const float* window, double frac, float& outL,
                                 float& outR) const noexcept {
    const double fp = frac * phases_;
    int p0 = static_cast<int>(fp);
    if (p0 >= phases_) p0 = phases_ - 1;
    const float t = static_cast<float>(fp - p0);
    const float* r0 = &table_[static_cast<size_t>(p0) * taps_];
    const float* r1 = r0 + taps_;
    float l = 0.0f, r = 0.0f;
    for (int k = 0; k < taps_; ++k) {
        const float c = r0[k] + t * (r1[k] - r0[k]);
        l += c * window[2 * k];
        r += c * window[2 * k + 1];
    }
    outL = l;
    outR = r;
}

} // namespace da
