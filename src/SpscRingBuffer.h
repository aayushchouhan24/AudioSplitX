#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace asx {

inline size_t nextPowerOfTwo(size_t value)
{
    if (value < 2) {
        return 2;
    }

    --value;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

template <typename T>
class SpscRingBuffer {
public:
    SpscRingBuffer() = default;

    explicit SpscRingBuffer(size_t capacityElements)
    {
        resetCapacity(capacityElements);
    }

    void resetCapacity(size_t capacityElements)
    {
        const size_t cap = nextPowerOfTwo(capacityElements);
        storage_.assign(cap, T{});
        mask_ = cap - 1;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const noexcept { return storage_.size(); }

    size_t readAvailable() const noexcept
    {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return w - r;
    }

    size_t writeAvailable() const noexcept
    {
        return capacity() - readAvailable();
    }

    void clear() noexcept
    {
        const size_t w = write_.load(std::memory_order_acquire);
        read_.store(w, std::memory_order_release);
    }

    size_t push(const T* input, size_t count) noexcept
    {
        if (count == 0 || storage_.empty()) {
            return 0;
        }

        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t available = capacity() - (w - r);
        const size_t toWrite = std::min(count, available);
        copyInto(w, input, toWrite);
        write_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t pop(T* output, size_t count) noexcept
    {
        if (count == 0 || storage_.empty()) {
            return 0;
        }

        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t available = w - r;
        const size_t toRead = std::min(count, available);
        copyOutOf(r, output, toRead);
        read_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    size_t discard(size_t count) noexcept
    {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t available = w - r;
        const size_t toDiscard = std::min(count, available);
        read_.store(r + toDiscard, std::memory_order_release);
        return toDiscard;
    }

private:
    void copyInto(size_t absoluteIndex, const T* input, size_t count) noexcept
    {
        const size_t first = std::min(count, capacity() - (absoluteIndex & mask_));
        std::memcpy(storage_.data() + (absoluteIndex & mask_), input, first * sizeof(T));
        if (count > first) {
            std::memcpy(storage_.data(), input + first, (count - first) * sizeof(T));
        }
    }

    void copyOutOf(size_t absoluteIndex, T* output, size_t count) noexcept
    {
        const size_t first = std::min(count, capacity() - (absoluteIndex & mask_));
        std::memcpy(output, storage_.data() + (absoluteIndex & mask_), first * sizeof(T));
        if (count > first) {
            std::memcpy(output + first, storage_.data(), (count - first) * sizeof(T));
        }
    }

    std::vector<T> storage_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> read_ { 0 };
    alignas(64) std::atomic<size_t> write_ { 0 };
};

} // namespace asx

