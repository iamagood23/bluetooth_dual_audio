#include "synchronization/LatencyCompensator.h"

#include <algorithm>

namespace da {

LatencyTargets ComputeLatencyTargets(const LatencyInputs& in) {
    LatencyTargets out;
    double base = 0.0;
    for (int i = 0; i < 2; ++i)
        if (in.rendered[i]) base = std::max(base, in.minAgeMs[i]);
    base += in.safetyMs + in.adaptiveMs;
    out.baseMs = base;

    // Reference offset: the smallest offset among outputs that are playing at all.
    double ref = 0.0;
    bool haveRef = false;
    for (int i = 0; i < 2; ++i) {
        if (!in.rendered[i] && !in.direct[i]) continue;
        if (in.direct[i]) {
            // A direct output is fixed; everything is relative to it.
            ref = in.offsetMs[i];
            haveRef = true;
            break;
        }
        ref = haveRef ? std::min(ref, in.offsetMs[i]) : in.offsetMs[i];
        haveRef = true;
    }
    for (int i = 0; i < 2; ++i) {
        if (!in.rendered[i]) continue;
        const double extra = haveRef ? std::max(0.0, in.offsetMs[i] - ref) : 0.0;
        out.extraDelayMs[i] = extra;
        out.targetMs[i] = base + extra;
    }
    return out;
}

} // namespace da
