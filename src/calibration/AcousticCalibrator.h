#pragma once
// Optional acoustic latency calibration.
//
// Windows reports latency only up to the Bluetooth driver; the codec and the earbuds' own
// buffering (often 100-250 ms and different per model) are invisible to it. The only honest
// way to measure that part is to listen: play a short chirp on one output at a time, record it
// with a microphone, and find when it actually arrived.
//
//  - Test chirps are injected into the master timeline at known positions, so we know exactly
//    when each output's device consumed them (IAudioClock), and the mic's QPC timestamps tell
//    us when they were heard. Unknown mic latency is common to both outputs and cancels out
//    in the *difference*, which is what synchronization needs.
//  - The mic is never a Bluetooth headset mic (opening one would force the earbuds into
//    hands-free mode) and never a virtual device.
//  - Nothing is recorded to disk. Results are accepted only if the repetitions agree and the
//    chirp clearly stands out from the noise; otherwise we report "unavailable".
#include "audio/AudioCapture.h"
#include "audio/AudioEngine.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace da {

struct CalibrationResult {
    bool success = false;
    std::wstring message;          // user-facing explanation
    double extraLatencyMs[2] = {0, 0}; // latency beyond what Windows reports, per output
    double spreadMs[2] = {0, 0};   // disagreement between repetitions
    double suggestedOffsetMs[2] = {0, 0};
    std::wstring microphoneName;
};

class AcousticCalibrator {
public:
    using Progress = std::function<void(const std::wstring& step, double fraction)>;

    // Blocking (run it on a worker thread). Both outputs must be rendered by `engine`.
    CalibrationResult Run(AudioEngine& engine, Progress progress, const std::atomic<bool>& cancel);

    // Chooses a microphone that is safe to open (not Bluetooth, not virtual). Empty if none.
    static std::wstring PickMicrophone(std::wstring* name);
};

} // namespace da
