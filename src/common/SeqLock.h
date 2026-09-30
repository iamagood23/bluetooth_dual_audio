#pragma once
// Single-writer / multi-reader sequence lock for small trivially-copyable snapshots.
// The writer never waits; readers retry if they observe a write in progress. Payload words are
// std::atomic so torn reads are well-defined (and then discarded by the sequence check).
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace da {

template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;

public:
    void Store(const T& value) noexcept {
        uint64_t words[kWords] = {};
        std::memcpy(words, &value, sizeof(T));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) data_[i].store(words[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Returns false only if a consistent copy could not be obtained after several attempts.
    bool Load(T& out) const noexcept {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint64_t words[kWords];
            for (size_t i = 0; i < kWords; ++i) words[i] = data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                std::memcpy(&out, words, sizeof(T));
                return s1 != 0; // never written -> no data
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> data_[kWords] = {};
};

} // namespace da
