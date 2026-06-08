#ifndef DEMULTIPLEXER_HPP
#define DEMULTIPLEXER_HPP

/**
 * @file demultiplexer.hpp
 *
 * Demultiplexer — Reactor pattern for the MCFlow dispatching subsystem.
 *
 * Role in the architecture (paper Figure 2, Section V-C)
 * -------------------------------------------------------
 * Each Dispatcher has an associated Demultiplexer that receives
 * asynchronous events (from eventfd / network I/O) and routes them to
 * the appropriate Dispatcher via the 6-step release-guard protocol.
 *
 * In the single-thread configuration (our current case), the Dispatcher
 * and Demultiplexer share the same thread. The Demultiplexer's process()
 * method encapsulates the 6-step logic so it can be reused independently
 * (e.g., for network-triggered subtasks or for the leader/followers
 * pattern where multiple threads share an epoll instance).
 *
 * 6-step protocol (Section V-C)
 * --------------------------------
 * 1. Thread removes a subtask from the FIFO subtask queue.
 * 2. Checks the atomic in_processing flag (guard for leader/followers).
 * 3. Checks whether release time for the subtask has expired.
 * 4a. If not expired → insert into timer queue; idle thread dispatches later.
 * 4b. If expired → set in_processing, execute.
 * 5.  After execution → notify all downstream subtasks.
 * 6.  Clear in_processing.
 */

#include "dispatcher.hpp"  // Subtask, TimerQueue, Dispatcher

class Demultiplexer {
public:
    /**
     * Process one subtask through the 6-step release-guard protocol.
     *
     * @param s           Subtask dequeued from the FIFO queue.
     * @param dispatcher  The owning Dispatcher (used to defer to timer queue
     *                    and to call process_subtask internally).
     *
     * Delegates entirely to Dispatcher::process_subtask() so there is a
     * single authoritative implementation of the 6 steps.
     */
    static void process(Subtask* s, Dispatcher& dispatcher) {
        dispatcher.process_subtask(s);
    }

    /**
     * Check whether all suppliers for subtask s have notified for this job.
     * Convenience wrapper around the fan_in state on the Subtask.
     */
    static bool all_suppliers_ready(const Subtask* s) {
        return s->fan_in_received.load(std::memory_order_acquire)
               >= s->fan_in_total;
    }
};

#endif // DEMULTIPLEXER_HPP
