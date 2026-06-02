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
 * Constraints
 * -----------
 * - N must be a power of two (enforced by static_assert).
 * - N must be greater than the pipeline depth (number of concurrent jobs).
 */

#include <atomic>
#include <cstddef>

/**
 * @brief Sequence-number-addressed ring buffer.
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

public:

    /**
     * @brief Write a value into the slot owned by job @p seq_num.
     *
     * Must be called by the producer *before* signalling the consumer
     * (e.g. before calling Dispatcher::notify()), so the consumer always
     * reads a fully written value.
     *
     * @param seq_num Monotonically increasing job counter maintained by the producer.
     * @param value   Value to store.
     */
    void write(size_t seq_num, const T& value) {
        buffer_[seq_num % N] = value;
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
     * After this call, the slot may be overwritten by job seq_num + N.
     * In this simplified version the call is a no-op; the full implementation
     * uses an atomic counter to prevent the producer from lapping the consumer.
     *
     * @param seq_num The consumer's current sequence counter.
     */
    void release(size_t seq_num) {
        (void)seq_num;
    }
};

#endif // RING_BUFFER_HPP
