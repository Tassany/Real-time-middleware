#pragma once
#ifndef PREEMPTIVE_TEAM_MANAGER_HPP
#define PREEMPTIVE_TEAM_MANAGER_HPP

/**
 * @file preemptive_team_manager.hpp
 *
 * PreemptiveTeamManager — drop-in replacement for TeamManager that uses
 * fully preemptive scheduling.
 *
 * The only architectural difference from TeamManager:
 *
 *   TeamManager groups subtasks by (core, priority) and creates one
 *   Dispatcher thread per unique pair.  Subtasks sharing a pair run
 *   non-preemptively from a FIFO queue in that thread.
 *
 *   PreemptiveTeamManager creates one PreemptiveDispatcher per subtask,
 *   each with its own SCHED_FIFO thread at the subtask's priority.  The
 *   Linux scheduler then enforces preemption: a higher-priority thread
 *   that becomes runnable on a core immediately preempts any lower-priority
 *   thread, even mid-execute().
 *
 * Everything else — fan-in wiring, downstream connections, ring-buffer
 * sizing, exception handling, state machine — is identical to TeamManager.
 *
 * Trade-offs vs. non-preemptive:
 *   + Higher-priority tasks are never blocked waiting for lower-priority ones
 *   + Scheduling complexity is delegated to the kernel
 *   - O(subtasks) threads instead of O(core × priority-levels)
 *   - More context-switch overhead on cores with many subtasks
 */

#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <stdexcept>
#include <iostream>
#include "dag.hpp"
#include "deployment_plan.hpp"
#include "preemptive_dispatcher.hpp"
#include "ring_buffer.hpp"

class PreemptiveTeamManager {
public:
    enum class State { CREATED, INITIALIZED, RUNNING, TERMINATING, TERMINATED };

    struct SubtaskEntry {
        SubtaskInfo info;
        Subtask*    subtask;
    };

    PreemptiveTeamManager();
    ~PreemptiveTeamManager();

    // Create one PreemptiveDispatcher per subtask, derive downstream
    // connections and fan_in_total from the DAG, and wrap each execute()
    // with exception handling.
    // Precondition: state == CREATED.
    void initialize(const std::vector<SubtaskEntry>& entries, const DAG& dag);

    // Start all dispatcher threads (one per subtask).
    // Precondition: state == INITIALIZED.
    void start();

    // Stop all dispatchers in reverse creation order (sinks first), then
    // transition to TERMINATED.  Idempotent.
    void stop();

    // Deliver one activation to the subtask identified by subtask_id.
    // Precondition: state == RUNNING.
    void notify(int subtask_id);

    // Called when a subtask's execute() throws.  Transitions to TERMINATING.
    // Thread-safe; may be called from a dispatcher thread.
    void on_subtask_exception(int subtask_id);

    State state() const;

    // Returns the number of PreemptiveDispatcher instances (== number of subtasks).
    std::size_t dispatcher_count() const;

    // Returns the recommended ring buffer slot count for the connection
    // upstream_id → downstream_id (same formula as TeamManager).
    std::size_t ring_buffer_size(int upstream_id, int downstream_id) const;

private:
    void do_stop();

    using Edge = std::pair<int, int>;  // (upstream_id, downstream_id)

    mutable std::mutex state_;
    State              state_val_;
    std::vector<int>   topo_order_;

    // One PreemptiveDispatcher per subtask, keyed by subtask_id
    std::map<int, std::unique_ptr<PreemptiveDispatcher>> dispatchers_;

    // Creation order (topological) — used to start/stop in the right sequence
    std::vector<int> dispatcher_order_;

    // subtask_id → its PreemptiveDispatcher* (raw pointer into dispatchers_)
    std::map<int, PreemptiveDispatcher*> subtask_dispatcher_;

    std::map<int, Subtask*>              subtasks_;
    std::map<Edge, std::size_t>          ring_buffer_sizes_;
};

#endif // PREEMPTIVE_TEAM_MANAGER_HPP
