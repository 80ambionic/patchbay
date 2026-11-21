#pragma once
// Wait-free single-producer / single-consumer queue with zero-copy slot access.
// Producer (network thread): acquireWrite() -> fill -> publish().
// Consumer (audio thread):   peek() -> use -> release().  Neither side ever blocks or allocates.
#include <atomic>
#include <cstddef>
#include <memory>

namespace pb::net
{
template <typename T, size_t N>
class SpscQueue
{
    static_assert ((N & (N - 1)) == 0 && N >= 2, "N must be a power of two");

public:
    SpscQueue() : slots (new T[N]) {}

    /** Producer: slot to fill, or nullptr if the queue is full (caller should count a drop). */
    T* acquireWrite() noexcept
    {
        const auto w = head.load (std::memory_order_relaxed);
        if (w - tail.load (std::memory_order_acquire) >= N) return nullptr;
        return &slots[w & (N - 1)];
    }
    void publish() noexcept { head.store (head.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

    /** Consumer: oldest unread slot, or nullptr if empty. */
    const T* peek() const noexcept
    {
        const auto r = tail.load (std::memory_order_relaxed);
        if (r == head.load (std::memory_order_acquire)) return nullptr;
        return &slots[r & (N - 1)];
    }
    void release() noexcept { tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
    std::unique_ptr<T[]> slots;
    alignas (64) std::atomic<size_t> head { 0 };   // written by producer
    alignas (64) std::atomic<size_t> tail { 0 };   // written by consumer
};
}
