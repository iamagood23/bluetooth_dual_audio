#pragma once
// Single-producer ring of interleaved stereo float frames addressed by *master timeline index*.
//
// The slot for master frame n is (n mod capacity). The producer (capture thread) only ever
// appends; the consumer (a render thread) reads at any index it likes inside the valid window
// [WriteIndex() - capacity + guard, WriteIndex()). Because the index *is* the timeline position,
// seeking (sync jumps) is just changing the consumer's read position.
//
// The first `guard` frames are mirrored past the end of the storage so a consumer can read
// `guard` contiguous frames starting at any slot without wrap-around logic (the resampler needs
// a contiguous window of taps).
//
// Never blocks, never allocates after Allocate().
#include <atomic>
#include <cstdint>
#include <vector>

namespace da {

class AudioRingBuffer {
public:
    static constexpr int kChannels = 2;

    // capacityFrames is rounded up to a power of two. Not real-time safe (allocates).
    void Allocate(uint32_t capacityFrames, uint32_t guardFrames);

    // Producer side ---------------------------------------------------------------------
    void Write(const float* interleaved, uint32_t frames) noexcept;
    void WriteSilence(uint32_t frames) noexcept;
    // Moves the write index without a reader present. Only call while no consumer is active.
    void ResetTo(int64_t masterIndex) noexcept;

    // Consumer side ---------------------------------------------------------------------
    // One past the newest complete frame (acquire).
    int64_t WriteIndex() const noexcept { return write_.load(std::memory_order_acquire); }
    // Oldest frame index that is guaranteed not to be overwritten by the next producer write.
    int64_t OldestSafeIndex() const noexcept {
        return WriteIndex() - static_cast<int64_t>(capacity_) + static_cast<int64_t>(guard_) +
               static_cast<int64_t>(kMaxProducerChunk);
    }
    // Pointer to frame `index`; `guard` frames after it are contiguous. Caller keeps the index
    // inside the valid window.
    const float* FramePtr(int64_t index) const noexcept {
        return data_.data() + (static_cast<uint64_t>(index) & mask_) * kChannels;
    }

    uint32_t Capacity() const noexcept { return capacity_; }
    uint32_t Guard() const noexcept { return guard_; }

    // The producer never writes more than this many frames in one call (it splits larger writes),
    // which bounds how far a torn read can reach. Public for OldestSafeIndex callers/tests.
    static constexpr uint32_t kMaxProducerChunk = 2048;

private:
    void WriteChunk(const float* src, uint32_t frames) noexcept;

    std::vector<float> data_;
    uint32_t capacity_ = 0;
    uint32_t guard_ = 0;
    uint64_t mask_ = 0;
    std::atomic<int64_t> write_{0};
};

} // namespace da
