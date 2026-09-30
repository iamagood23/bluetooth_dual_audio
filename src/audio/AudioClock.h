#pragma once
// Timing records shared between the capture ("master timeline") thread, the render threads and
// the synchronization controller. All times are QPC-derived 100 ns units (see common/Clock.h).
#include <cstdint>

namespace da {

// Published by the capture thread for every packet: master frame `masterIndex` was captured
// at `hns`. Together with the timeline rate this maps any master frame to a wall-clock time.
struct CaptureAnchor {
    int64_t masterIndex = 0;
    int64_t hns = 0;
    double rate = 48000.0;
    int64_t pad = 0;
};

// Published by a render thread after each buffer: at wall time `hns`, the device was playing
// master timeline position `masterPos` (fractional; resampled), and had played
// `devicePosFrames` device frames since the stream started (IAudioClock).
struct RenderTiming {
    int64_t hns = 0;
    double masterPos = 0.0;
    double devicePosFrames = 0.0;
    uint32_t paddingFrames = 0;
    uint32_t streamGeneration = 0; // increments every time the stream is (re)started
    // Timeline position the render thread will read next. (readPos - masterPos) is the audio
    // queued inside Windows/the driver: the device's real pipeline depth.
    double readPos = 0.0;
};

// Maps a master frame to its capture time using the latest anchor.
inline int64_t MasterToHns(const CaptureAnchor& a, double masterPos) noexcept {
    return a.hns - static_cast<int64_t>((static_cast<double>(a.masterIndex) - masterPos) * 1e7 / a.rate);
}

} // namespace da
