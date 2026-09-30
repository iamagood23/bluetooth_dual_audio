#pragma once
// One timebase for the whole engine: 100-nanosecond units derived from QueryPerformanceCounter.
// This is the same unit WASAPI uses for IAudioClock::GetPosition's qpc position and
// IAudioCaptureClient::GetBuffer's pu64QPCPosition, so timestamps compare directly.
#include <windows.h>
#include <cstdint>

namespace da {

inline int64_t QpcFrequency() noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return f;
}

inline int64_t QpcToHns(int64_t qpc) noexcept {
    const int64_t f = QpcFrequency();
    // Split to avoid overflowing 64 bits on long uptimes.
    return (qpc / f) * 10'000'000 + (qpc % f) * 10'000'000 / f;
}

inline int64_t NowHns() noexcept {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return QpcToHns(li.QuadPart);
}

constexpr int64_t kHnsPerSecond = 10'000'000;
constexpr int64_t kHnsPerMs = 10'000;

inline double HnsToMs(int64_t hns) noexcept { return static_cast<double>(hns) / kHnsPerMs; }

} // namespace da
