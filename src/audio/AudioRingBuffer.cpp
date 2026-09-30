#include "audio/AudioRingBuffer.h"

#include <algorithm>
#include <cstring>

namespace da {

void AudioRingBuffer::Allocate(uint32_t capacityFrames, uint32_t guardFrames) {
    uint32_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    capacity_ = cap;
    guard_ = std::min(guardFrames, cap / 2);
    mask_ = cap - 1;
    data_.assign(static_cast<size_t>(cap + guard_) * kChannels, 0.0f);
    write_.store(0, std::memory_order_release);
}

void AudioRingBuffer::ResetTo(int64_t masterIndex) noexcept {
    std::fill(data_.begin(), data_.end(), 0.0f);
    write_.store(masterIndex, std::memory_order_release);
}

void AudioRingBuffer::WriteChunk(const float* src, uint32_t frames) noexcept {
    const int64_t w = write_.load(std::memory_order_relaxed);
    uint64_t slot = static_cast<uint64_t>(w) & mask_;
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t run = std::min<uint32_t>(frames - done, static_cast<uint32_t>(capacity_ - slot));
        float* dst = data_.data() + slot * kChannels;
        if (src)
            std::memcpy(dst, src + static_cast<size_t>(done) * kChannels, run * kChannels * sizeof(float));
        else
            std::memset(dst, 0, run * kChannels * sizeof(float));
        // Mirror the head of the ring into the guard area.
        if (slot < guard_) {
            const uint32_t m = std::min<uint32_t>(run, static_cast<uint32_t>(guard_ - slot));
            std::memcpy(data_.data() + (capacity_ + slot) * kChannels, dst, m * kChannels * sizeof(float));
        }
        done += run;
        slot = (slot + run) & mask_;
    }
    write_.store(w + frames, std::memory_order_release);
}

void AudioRingBuffer::Write(const float* interleaved, uint32_t frames) noexcept {
    while (frames) {
        const uint32_t n = std::min(frames, kMaxProducerChunk);
        WriteChunk(interleaved, n);
        interleaved += static_cast<size_t>(n) * kChannels;
        frames -= n;
    }
}

void AudioRingBuffer::WriteSilence(uint32_t frames) noexcept {
    while (frames) {
        const uint32_t n = std::min(frames, kMaxProducerChunk);
        WriteChunk(nullptr, n);
        frames -= n;
    }
}

} // namespace da
