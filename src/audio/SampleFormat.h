#pragma once
// Conversion between the engine's internal format (interleaved stereo float32) and whatever
// shared-mode format a Windows endpoint exposes.
#include <windows.h>
#include <mmreg.h>
#include <cstdint>
#include <string>

namespace da {

enum class SampleType { Float32, Int16, Int24Packed, Int32 };

struct DeviceFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
    uint16_t validBits = 0;
    SampleType type = SampleType::Float32;

    // Returns false for formats we cannot convert (compressed, exotic bit depths).
    static bool FromWaveFormat(const WAVEFORMATEX* wf, DeviceFormat& out) noexcept;
    std::wstring Describe() const; // e.g. "48 kHz · 2 ch · 32-bit float"
};

// Internal stereo float -> device buffer. Channel mapping: mono devices get (L+R)/2,
// stereo devices get L/R, multichannel devices get L/R in the first two channels and silence
// elsewhere (the Windows channel mask puts FL/FR first).
class SampleWriter {
public:
    void Configure(const DeviceFormat& fmt) noexcept { fmt_ = fmt; }
    void Write(const float* stereo, uint32_t frames, BYTE* dst) noexcept;
    uint64_t ClampedSamples() const noexcept { return clamped_; }

private:
    float Dither() noexcept; // TPDF, +/-1 LSB of 16-bit
    DeviceFormat fmt_;
    uint32_t rng_ = 0x12345678u;
    uint64_t clamped_ = 0;
};

// Device/capture buffer -> internal stereo float. Mono is duplicated, multichannel keeps the
// first two channels.
class SampleReader {
public:
    void Configure(const DeviceFormat& fmt) noexcept { fmt_ = fmt; }
    void Read(const BYTE* src, uint32_t frames, float* stereo) const noexcept;

private:
    DeviceFormat fmt_;
};

} // namespace da
