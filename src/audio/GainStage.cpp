#include "audio/GainStage.h"

#include <cmath>

namespace da {

namespace {
constexpr float kCeiling = 0.999f; // just below full scale
}

void GainStage::Configure(double sampleRate) noexcept {
    // Volume changes ramp over ~10 ms (full 0->1 swing) to avoid zipper noise.
    rampStep_ = static_cast<float>(1.0 / (0.010 * sampleRate));
    // Limiter release time constant ~60 ms.
    release_ = static_cast<float>(std::exp(-1.0 / (0.060 * sampleRate)));
}

float GainStage::SliderToGain(float slider) noexcept {
    if (slider <= 0.0f) return 0.0f;
    if (slider >= 1.0f) return 1.0f;
    // Square-law taper: 50% -> -12 dB, 75% -> -5 dB.
    return slider * slider;
}

void GainStage::Process(float* s, uint32_t frames, float target, bool clipProtection) noexcept {
    for (uint32_t i = 0; i < frames; ++i) {
        if (gain_ != target) {
            const float d = target - gain_;
            gain_ = (std::fabs(d) <= rampStep_) ? target : gain_ + (d > 0 ? rampStep_ : -rampStep_);
        }
        float l = s[2 * i] * gain_;
        float r = s[2 * i + 1] * gain_;
        const float peak = std::fmax(std::fabs(l), std::fabs(r));
        if (peak > 1.0f) ++overs_;
        if (clipProtection) {
            if (peak * reduction_ > kCeiling) {
                reduction_ = kCeiling / peak; // instant attack
            } else if (reduction_ < 1.0f) {
                reduction_ = 1.0f - (1.0f - reduction_) * release_;
                if (reduction_ > 0.99999f) reduction_ = 1.0f;
            }
            if (reduction_ < 1.0f) {
                ++limited_;
                l *= reduction_;
                r *= reduction_;
            }
        }
        s[2 * i] = l;
        s[2 * i + 1] = r;
    }
}

} // namespace da
