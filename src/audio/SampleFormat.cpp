#include "audio/SampleFormat.h"

#include "common/StringUtil.h"

#include <ks.h>
#include <ksmedia.h>
#include <cmath>
#include <cstring>

namespace da {

bool DeviceFormat::FromWaveFormat(const WAVEFORMATEX* wf, DeviceFormat& out) noexcept {
    if (!wf || wf->nChannels == 0 || wf->nSamplesPerSec == 0) return false;
    WORD tag = wf->wFormatTag;
    WORD valid = wf->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            tag = WAVE_FORMAT_PCM;
        else
            return false;
        if (ext->Samples.wValidBitsPerSample) valid = ext->Samples.wValidBitsPerSample;
    }
    out.sampleRate = wf->nSamplesPerSec;
    out.channels = wf->nChannels;
    out.blockAlign = wf->nBlockAlign;
    out.validBits = valid;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) {
        out.type = SampleType::Float32;
    } else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16) {
        out.type = SampleType::Int16;
    } else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 24) {
        out.type = SampleType::Int24Packed;
    } else if (tag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32) {
        out.type = SampleType::Int32; // includes 24-in-32 containers
    } else {
        return false;
    }
    return out.blockAlign == out.channels * (wf->wBitsPerSample / 8);
}

std::wstring DeviceFormat::Describe() const {
    const wchar_t* t = L"?";
    switch (type) {
    case SampleType::Float32: t = L"32-bit float"; break;
    case SampleType::Int16: t = L"16-bit"; break;
    case SampleType::Int24Packed: t = L"24-bit"; break;
    case SampleType::Int32: t = validBits == 24 ? L"24-bit (32 container)" : L"32-bit"; break;
    }
    const double khz = sampleRate / 1000.0;
    return FormatW(khz == std::floor(khz) ? L"%.0f kHz · %u ch · %s" : L"%.1f kHz · %u ch · %s", khz,
                   channels, t);
}

float SampleWriter::Dither() noexcept {
    // Two uniform variables -> triangular PDF in (-1, 1) LSB.
    rng_ = rng_ * 1664525u + 1013904223u;
    const float a = static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
    rng_ = rng_ * 1664525u + 1013904223u;
    const float b = static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
    return a - b;
}

void SampleWriter::Write(const float* s, uint32_t frames, BYTE* dst) noexcept {
    const uint16_t ch = fmt_.channels;
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = s[2 * i], r = s[2 * i + 1];
        float v[2] = {l, r};
        const int mapped = ch == 1 ? 1 : 2;
        if (ch == 1) v[0] = 0.5f * (l + r);
        BYTE* frame = dst + static_cast<size_t>(i) * fmt_.blockAlign;
        switch (fmt_.type) {
        case SampleType::Float32: {
            float* f = reinterpret_cast<float*>(frame);
            for (int c = 0; c < mapped; ++c) f[c] = v[c];
            for (int c = mapped; c < ch; ++c) f[c] = 0.0f;
            break;
        }
        case SampleType::Int16: {
            int16_t* p = reinterpret_cast<int16_t*>(frame);
            for (int c = 0; c < mapped; ++c) {
                float x = v[c] * 32767.0f + Dither();
                if (x > 32767.0f) { x = 32767.0f; ++clamped_; }
                if (x < -32768.0f) { x = -32768.0f; ++clamped_; }
                p[c] = static_cast<int16_t>(std::lrintf(x));
            }
            for (int c = mapped; c < ch; ++c) p[c] = 0;
            break;
        }
        case SampleType::Int24Packed: {
            for (int c = 0; c < ch; ++c) {
                int32_t q = 0;
                if (c < mapped) {
                    float x = v[c] * 8388607.0f;
                    if (x > 8388607.0f) { x = 8388607.0f; ++clamped_; }
                    if (x < -8388608.0f) { x = -8388608.0f; ++clamped_; }
                    q = static_cast<int32_t>(std::lrintf(x));
                }
                BYTE* b = frame + c * 3;
                b[0] = static_cast<BYTE>(q & 0xFF);
                b[1] = static_cast<BYTE>((q >> 8) & 0xFF);
                b[2] = static_cast<BYTE>((q >> 16) & 0xFF);
            }
            break;
        }
        case SampleType::Int32: {
            int32_t* p = reinterpret_cast<int32_t*>(frame);
            for (int c = 0; c < mapped; ++c) {
                double x = static_cast<double>(v[c]) * 2147483647.0;
                if (x > 2147483647.0) { x = 2147483647.0; ++clamped_; }
                if (x < -2147483648.0) { x = -2147483648.0; ++clamped_; }
                p[c] = static_cast<int32_t>(std::llround(x));
            }
            for (int c = mapped; c < ch; ++c) p[c] = 0;
            break;
        }
        }
    }
}

void SampleReader::Read(const BYTE* src, uint32_t frames, float* out) const noexcept {
    const uint16_t ch = fmt_.channels;
    for (uint32_t i = 0; i < frames; ++i) {
        const BYTE* frame = src + static_cast<size_t>(i) * fmt_.blockAlign;
        float v[2] = {0.0f, 0.0f};
        const int n = ch >= 2 ? 2 : 1;
        for (int c = 0; c < n; ++c) {
            switch (fmt_.type) {
            case SampleType::Float32: v[c] = reinterpret_cast<const float*>(frame)[c]; break;
            case SampleType::Int16: v[c] = reinterpret_cast<const int16_t*>(frame)[c] / 32768.0f; break;
            case SampleType::Int24Packed: {
                const BYTE* b = frame + c * 3;
                int32_t q = (static_cast<int32_t>(b[2]) << 24) | (b[1] << 16) | (b[0] << 8);
                v[c] = static_cast<float>(q / 2147483648.0);
                break;
            }
            case SampleType::Int32:
                v[c] = static_cast<float>(reinterpret_cast<const int32_t*>(frame)[c] / 2147483648.0);
                break;
            }
        }
        if (ch == 1) v[1] = v[0];
        out[2 * i] = v[0];
        out[2 * i + 1] = v[1];
    }
}

} // namespace da
