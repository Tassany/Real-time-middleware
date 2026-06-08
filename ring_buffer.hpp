#ifndef RING_BUFFER_HPP
#define RING_BUFFER_HPP

/**
 * @file ring_buffer.hpp
 *
 * Provides two lock-free ring buffer types for inter-subtask communication,
 * following the MCFlow paper (Huang et al., 2012).
 *
 * Key design points
 * -----------------
 * - Each slot is aligned to and padded up to a full cache-line boundary
 *   (alignas(64)) to eliminate false sharing between adjacent slots.
 * - The consumer position counter lives on its own cache line, separate from
 *   the slots array, to avoid false sharing between the producer (reading
 *   consumer_pos_ for backpressure) and the consumer (writing it).
 * - Backpressure: write() spins until the target slot is free, i.e. until
 *   the consumer has called release() for the oldest outstanding job.
 *   N must therefore be larger than the maximum pipeline depth so that
 *   blocking is rare under normal conditions.
 *
 * RingBuffer<T, N>
 *   Single-producer / single-consumer (SPSC).
 *
 * MultiSupplierRingBuffer<T, N, NumSuppliers>
 *   Multiple producers / single consumer (fan-in).
 *   Each slot has one field per supplier; the slot is ready for consumption
 *   only when every supplier has written (tracked via an atomic bitmask).
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

static constexpr size_t CACHE_LINE_SIZE = 64; // Specific for x86-64

// ============================================================
//  RingBuffer<T, N> sequence-number addressed
// ============================================================

template<typename T, size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2,             "N must be at least 2");

    // alignas(64) forces sizeof(Slot) to be a multiple of 64.
    struct alignas(CACHE_LINE_SIZE) Slot { T data; };
    static_assert(sizeof(Slot) % CACHE_LINE_SIZE == 0,
                  "Slot must be a multiple of the cache-line size");

    Slot slots_[N];

    // On its own cache line: written by consumer, read by producer.
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> consumer_pos_{0};

public:
    /**
     * Write value for job seq_num.
     * Spins (backpressure) until the slot is free.
     * Must be called before notifying the consumer dispatcher.
     */
    void write(size_t seq_num, const T& value) {
        while (seq_num >= consumer_pos_.load(std::memory_order_acquire) + N)
            ; // spin: producer waits until consumer releases an old slot
        slots_[seq_num & (N - 1)].data = value;
    }

    /**
     * Return a reference to the slot for job seq_num.
     * The reference is valid until release() is called for this seq_num.
     */
    T& read(size_t seq_num) {
        return slots_[seq_num & (N - 1)].data;
    }

    /**
     * Mark slot as consumed; allows the producer to reuse it for job
     * seq_num + N.  Must be called after the consumer is done with the data.
     */
    void release(size_t seq_num) {
        consumer_pos_.store(seq_num + 1, std::memory_order_release);
    }

    // Slot size in bytes — useful for tests and static_assert verification.
    static constexpr size_t slot_size() { return sizeof(Slot); }
};


// ============================================================
//  MultiSupplierRingBuffer<T, N, NumSuppliers>  —  fan-in
// ============================================================

template<typename T, size_t N, size_t NumSuppliers>
class MultiSupplierRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2,             "N must be at least 2");
    static_assert(NumSuppliers >= 1 && NumSuppliers <= 64,
                  "NumSuppliers must be in [1, 64]");

    static constexpr uint64_t FULL_MASK =
        (NumSuppliers == 64) ? ~uint64_t(0)
                             : ((uint64_t(1) << NumSuppliers) - 1);

    // Each slot stores one data field per supplier plus an atomic bitmask.
    struct alignas(CACHE_LINE_SIZE) Slot {
        T data[NumSuppliers];
        std::atomic<uint64_t> ready_mask{0};
    };
    static_assert(sizeof(Slot) % CACHE_LINE_SIZE == 0,
                  "Slot must be a multiple of the cache-line size");

    Slot slots_[N];
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> consumer_pos_{0};

public:
    /**
     * Supplier supplier_id (0-based) writes its value for job seq_num.
     * Sets the corresponding bit in the slot's ready_mask.
     * Spins (backpressure) until the slot is free.
     */
    void write(size_t seq_num, size_t supplier_id, const T& value) {
        while (seq_num >= consumer_pos_.load(std::memory_order_acquire) + N)
            ;
        Slot& slot = slots_[seq_num & (N - 1)];
        slot.data[supplier_id] = value;
        slot.ready_mask.fetch_or(uint64_t(1) << supplier_id,
                                 std::memory_order_release);
    }

    /**
     * Returns true when all NumSuppliers have written their value for seq_num.
     */
    bool ready(size_t seq_num) const {
        return slots_[seq_num & (N - 1)].ready_mask.load(
                   std::memory_order_acquire) == FULL_MASK;
    }

    /**
     * Access the value written by supplier_id for job seq_num.
     * Call only after ready() returns true.
     */
    const T& read(size_t seq_num, size_t supplier_id) const {
        return slots_[seq_num & (N - 1)].data[supplier_id];
    }

    /**
     * Consumer is done with this slot: reset the bitmask and advance
     * consumer_pos_ so the suppliers can reuse the slot.
     */
    void release(size_t seq_num) {
        slots_[seq_num & (N - 1)].ready_mask.store(0,
            std::memory_order_release);
        consumer_pos_.store(seq_num + 1, std::memory_order_release);
    }

    static constexpr size_t slot_size() { return sizeof(Slot); }
};

#endif // RING_BUFFER_HPP
