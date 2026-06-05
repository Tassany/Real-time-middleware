#ifndef RING_BUFFER_HPP
#define RING_BUFFER_HPP

/**
 * @file ring_buffer.hpp
 * @brief Lock-free circular buffer for zero-copy inter-subtask communication.
 *
 * Design (sequence-number scheme)
 * --------------------------------
 * Rather than shared head/tail pointers, each job carries a monotonically
 * increasing sequence number.  The slot index is simply:
 *
 *     slot = seq_num % N
 *
 * Because producer and consumer advance their own counters in lockstep, and
 * N is larger than the maximum number of jobs simultaneously in flight, they
 * never write to the same slot at the same time — no mutex needed.
 *
 * Producer-consumer coordination
 * --------------------------------
 * Two cache-line-aligned atomic counters track progress:
 *   written_  — incremented by the producer after each write()
 *   released_ — incremented by the consumer after each release()
 *
 * The producer checks that it is not lapping the consumer:
 *   write() returns false (buffer full) if seq_num - released_ >= N
 *
 * Each counter lives on its own cache line to avoid false sharing (§V-B).
 *
 * Constraints
 * -----------
 * - N must be a power of two (enforced by static_assert).
 * - N must be greater than the pipeline depth (number of concurrent jobs).
 */

#include <atomic>
#include <cstddef>

/**
 * @brief Sequence-number-addressed lock-free ring buffer.
 *
 * @tparam T Type of elements stored. Must be trivially copyable for safe
 *           concurrent access without explicit synchronisation.
 * @tparam N Capacity. Must be a power of two and at least 2.
 */
template<typename T, size_t N>
class RingBuffer {

    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2,             "N must be at least 2");

    T buffer_[N];

    // Each counter on its own cache line to prevent false sharing (§V-B).
    struct alignas(64) Counter { std::atomic<size_t> v{0}; };
    Counter written_;   ///< Number of slots written by the producer.
    Counter released_;  ///< Number of slots released by the consumer.

public:

    /**
     * @brief Write a value into the slot owned by job @p seq_num.
     *
     * Returns false if the buffer is full (consumer has not released enough
     * slots). The producer must not advance past the consumer by more than N.
     *
     * Must be called before signalling the consumer (e.g. before notify()),
     * so the consumer always reads a fully written value.
     *
     * @param seq_num Monotonically increasing job counter maintained by the producer.
     * @param value   Value to store.
     * @return true if the write succeeded; false if the buffer is full.
     */
    bool write(size_t seq_num, const T& value) {
        if (seq_num - released_.v.load(std::memory_order_acquire) >= N)
            return false;
        buffer_[seq_num % N] = value;
        written_.v.store(seq_num + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Return a direct reference to the slot owned by job @p seq_num.
     *
     * Returns a reference rather than a copy to allow the consumer to work
     * directly on the buffer, avoiding an extra allocation.
     *
     * @param seq_num The same counter used to write the value.
     * @return Reference to the stored value. Valid until release() is called.
     */
    T& read(size_t seq_num) {
        return buffer_[seq_num % N];
    }

    /**
     * @brief Mark the slot for job @p seq_num as free.
     *
     * Advances the released counter so the producer knows it may reuse the
     * slot. Must be called after the consumer is done reading the value.
     *
     * @param seq_num The consumer's current sequence counter.
     */
    void release(size_t seq_num) {
        released_.v.store(seq_num + 1, std::memory_order_release);
    }

    /**
     * @brief Return true if data has been written for @p seq_num.
     *
     * Used by the Demultiplexer step 5 to drain pending inputs: keep
     * executing while has_data(consumer_seq) is true.
     *
     * @param seq_num The consumer's current sequence counter.
     */
    bool has_data(size_t seq_num) const {
        return written_.v.load(std::memory_order_acquire) > seq_num;
    }
};

#endif // RING_BUFFER_HPP
