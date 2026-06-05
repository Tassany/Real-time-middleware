#pragma once
#ifndef TEAM_MANAGER_HPP
#define TEAM_MANAGER_HPP

/**
 * @file team_manager.hpp
 * @brief TeamManager: lifecycle controller for a set of co-located subtasks.
 *
 * A "team" groups the subtasks assigned to a single host. The TeamManager
 * drives them through a well-defined lifecycle and owns all Demultiplexers
 * and SubtaskDescriptors for the host.
 *
 * Phase 2 migration: replaced Dispatcher + Subtask with Demultiplexer +
 * SubtaskDescriptor so that the release guard, timer queue, and fan-in
 * logic are active in the main pipeline (not just in standalone examples).
 */

#include "demultiplexer.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <unistd.h>

enum TeamState {
    TEAM_CREATED,
    TEAM_INITIALIZED,
    TEAM_RUNNING,
    TEAM_TERMINATING,
    TEAM_TERMINATED
};

struct SubtaskEntry {
    SubtaskDescriptor* desc;
    Demultiplexer*     demux;
};

// ================================================================
//  TeamManager
//
//  Manages the lifecycle of a set of subtasks on a single host.
//
//  Typical usage:
//
//    TeamManager tm("sensor_task");
//
//    Demultiplexer* d0 = tm.add_demultiplexer(0, 50);
//    Demultiplexer* d1 = tm.add_demultiplexer(1, 49);
//
//    SubtaskDescriptor* supplier = tm.add_subtask("Supplier", d0, []{ ... });
//    SubtaskDescriptor* consumer = tm.add_subtask("Consumer", d1, []{ ... });
//
//    tm.add_connection(supplier, consumer);
//
//    tm.start();
//    tm.trigger(supplier);
//    ...
//    tm.stop();
// ================================================================

class TeamManager {
public:

    explicit TeamManager(const std::string& name)
        : name_(name), state_(TEAM_CREATED) {}

    ~TeamManager() {
        if (state_ == TEAM_RUNNING)
            stop();
        for (auto* d : demuxes_) delete d;
        for (auto& e : entries_) delete e.desc;
    }

    // -------------------------------------------------------
    //  add_demultiplexer(core, priority)
    //
    //  Creates a Demultiplexer for the given (core, priority) pair.
    //  Returns a pointer so you can assign subtasks to it.
    // -------------------------------------------------------
    Demultiplexer* add_demultiplexer(int core, int priority) {
        auto* d = new Demultiplexer(core, priority);
        demuxes_.push_back(d);
        return d;
    }

    // -------------------------------------------------------
    //  add_subtask(name, demux, fn)
    //
    //  Creates a SubtaskDescriptor, registers it with the given
    //  Demultiplexer, and returns the descriptor pointer.
    // -------------------------------------------------------
    SubtaskDescriptor* add_subtask(const std::string&    name,
                                   Demultiplexer*        demux,
                                   std::function<void()> fn)
    {
        auto* desc = new SubtaskDescriptor{};
        desc->name    = name;
        desc->execute = fn;
        demux->register_subtask(desc);
        entries_.push_back({desc, demux});
        return desc;
    }

    // -------------------------------------------------------
    //  add_connection(upstream, downstream, transfer_fn)
    //
    //  Wires upstream → downstream with optional data transfer.
    //
    //  Fan-in: downstream->supplier_count is incremented per call.
    //  The upstream lambda uses an atomic countdown: it writes to
    //  downstream->efd only when the last supplier arrives.
    //  For a single supplier, this reduces to the original behaviour.
    // -------------------------------------------------------
    void add_connection(SubtaskDescriptor*    upstream,
                        SubtaskDescriptor*    downstream,
                        std::function<void()> transfer_fn = {})
    {
        downstream->supplier_count++;

        auto original_fn = upstream->execute;
        upstream->execute = [original_fn, downstream, transfer_fn]() {
            original_fn();
            if (transfer_fn) transfer_fn();

            int remaining = downstream->pending_suppliers.fetch_sub(
                1, std::memory_order_acq_rel) - 1;

            if (remaining == 0) {
                // All suppliers delivered — reset counter and wake consumer.
                downstream->pending_suppliers.store(
                    downstream->supplier_count, std::memory_order_release);
                uint64_t sig = 1;
                ::write(downstream->efd, &sig, sizeof(sig));
            }
        };
    }

    // -------------------------------------------------------
    //  set_period(desc, period_ns)
    //
    //  Marks a subtask as periodic and sets its period directly
    //  on the SubtaskDescriptor (no intermediate SubtaskEntry).
    // -------------------------------------------------------
    void set_period(SubtaskDescriptor* desc, long period_ns) {
        desc->period_ns   = period_ns;
        desc->is_periodic = true;
    }

    // -------------------------------------------------------
    //  trigger(desc)
    //
    //  Sends the first job into the pipeline by writing to the
    //  subtask's eventfd. The Demultiplexer picks it up via epoll.
    //  Call this after start() to kick off source subtasks.
    // -------------------------------------------------------
    void trigger(SubtaskDescriptor* desc) {
        if (desc->efd < 0)
            throw std::logic_error(
                "subtask efd not initialised — was start() called?");
        uint64_t sig = 1;
        ::write(desc->efd, &sig, sizeof(sig));
    }

    // -------------------------------------------------------
    //  start() — CREATED -> INITIALIZED -> RUNNING
    // -------------------------------------------------------
    void start() {
        std::cout << "[TeamManager] " << name_ << " starting\n";
        state_ = TEAM_INITIALIZED;

        // Initialise fan-in counters before demultiplexers start.
        for (auto& e : entries_) {
            if (e.desc->supplier_count > 0)
                e.desc->pending_suppliers.store(
                    e.desc->supplier_count, std::memory_order_relaxed);
        }

        for (auto* d : demuxes_)
            d->start();

        state_ = TEAM_RUNNING;
        std::cout << "[TeamManager] " << name_ << " running\n";
    }

    // -------------------------------------------------------
    //  stop() — RUNNING -> TERMINATING -> TERMINATED
    // -------------------------------------------------------
    void stop() {
        if (state_ != TEAM_RUNNING) return;

        std::cout << "[TeamManager] " << name_ << " stopping\n";
        state_ = TEAM_TERMINATING;

        for (auto* d : demuxes_)
            d->stop();

        state_ = TEAM_TERMINATED;
        std::cout << "[TeamManager] " << name_ << " terminated\n";
    }

    TeamState state() const { return state_; }

private:

    std::string                  name_;
    TeamState                    state_;
    std::vector<Demultiplexer*>  demuxes_;
    std::vector<SubtaskEntry>    entries_;
};

#endif // TEAM_MANAGER_HPP
