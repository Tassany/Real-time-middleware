#pragma once

/**
 * @file preemptive_dispatcher.hpp
 *
 * Fully preemptive dispatcher: one OS thread per subtask.
 *
 * Why this achieves true preemption
 * -----------------------------------
 * The original Dispatcher assigns one thread to all subtasks sharing a
 * (core, priority) pair and processes them from a FIFO queue — non-preemptive
 * within that priority level.
 *
 * Here, each subtask owns exactly one SCHED_FIFO thread pinned to its core.
 * When a higher-priority thread on the same core becomes runnable (because
 * its fan-in condition was met and notify() wrote to its eventfd), the Linux
 * SCHED_FIFO scheduler immediately preempts any lower-priority thread mid-
 * execute().  No scheduling code is needed: the kernel does it.
 *
 * Periodic release guard — timerfd + epoll (no idle thread)
 * ----------------------------------------------------------
 * The original Dispatcher uses a dedicated idle thread to monitor a timer
 * queue and re-enqueue deferred subtasks.  It needs the idle thread because
 * one Dispatcher thread serves N subtasks: it cannot block waiting for a
 * timer while other subtasks in the same queue need to execute.
 *
 * Here, each thread serves exactly ONE subtask, so it can stay in
 * epoll_wait() watching both:
 *   efd_     — eventfd (EFD_SEMAPHORE) written by notify() on each fan-in
 *   timerfd_ — absolute-time kernel timer for the periodic release guard
 *
 * When a notification arrives before next_release_ns, the thread arms
 * timerfd_ with TFD_TIMER_ABSTIME and goes back to epoll_wait().  The kernel
 * fires the timerfd interrupt at exactly next_release_ns; epoll_wait() wakes,
 * and the subtask executes.  Precision matches the original Dispatcher's
 * timerfd approach without a second thread.
 *
 * EFD_SEMAPHORE accumulates tokens if fan-in is satisfied again while the
 * thread is busy: each token represents one pending activation and is
 * drained one-per-iteration.
 */

#include <atomic>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "dispatcher.hpp"  // Subtask, SubtaskConn, IDispatcher

class PreemptiveDispatcher : public IDispatcher {
public:
    PreemptiveDispatcher(Subtask* s, int core, int priority)
        : subtask_(s), core_(core), priority_(priority),
          efd_(-1), timerfd_(-1), epfd_(-1), running_(false) {}

    ~PreemptiveDispatcher() { stop(); }

    /**
     * Signal that all preconditions for subtask s are met for one job.
     *
     * Implements the same fan-in gate as Dispatcher::notify().
     * Thread-safe; may be called from any thread.
     */
    void notify(Subtask* s) override {
        int received = s->fan_in_received.fetch_add(1) + 1;
        if (received < s->fan_in_total) return;
        s->fan_in_received.store(0);

        uint64_t sig = 1;
        ::write(efd_, &sig, sizeof(sig));
    }

    void start() {
        efd_     = eventfd(0, EFD_SEMAPHORE);
        timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        epfd_    = epoll_create1(0);

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, efd_, &ev);
        ev.data.fd = timerfd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, timerfd_, &ev);

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        // Wake epoll_wait so the thread can observe running_ == false
        uint64_t wake = 1;
        ::write(efd_, &wake, sizeof(wake));
        pthread_join(thread_, nullptr);

        close(efd_);     efd_     = -1;
        close(timerfd_); timerfd_ = -1;
        close(epfd_);    epfd_    = -1;
    }

    static uint64_t monotonic_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

private:
    void loop() {
        // Pin to designated core
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        // Set SCHED_FIFO priority — this is what enables OS-level preemption:
        // if a higher-priority thread on this core becomes runnable it will
        // immediately preempt this thread, even mid-execute().
        struct sched_param param{};
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
            std::cerr << "[PreemptiveDispatcher core=" << core_
                      << " prio=" << priority_
                      << "] warning: RT priority not applied (run with sudo)\n";

        std::cerr << "[PreemptiveDispatcher] core=" << core_
                  << " prio=" << priority_
                  << " subtask=" << subtask_->id << " started\n";

        Subtask* s = subtask_;
        struct epoll_event events[2];

        while (running_) {
            // Block until either a fan-in notification (efd_) or the periodic
            // release timer (timerfd_) fires.  Infinite timeout: the stop()
            // shutdown path writes to efd_ to unblock this call.
            int n = epoll_wait(epfd_, events, 2, /*timeout_ms=*/-1);
            if (n <= 0) continue;

            bool got_efd   = false;
            bool got_timer = false;

            for (int i = 0; i < n; ++i) {
                uint64_t val;
                ::read(events[i].data.fd, &val, sizeof(val));  // drain the fd
                if (events[i].data.fd == efd_)     got_efd   = true;
                if (events[i].data.fd == timerfd_) got_timer = true;
            }

            if (!running_) break;

            bool should_execute = false;

            if (got_efd) {
                // Periodic release guard: if the notification arrived before
                // next_release_ns, arm timerfd_ at the exact release time and
                // go back to epoll_wait().  The kernel will fire the interrupt
                // precisely at next_release_ns — no busy-wait, no overshoot
                // from clock_nanosleep on non-RT kernels.
                if (s->period_ns > 0 && s->next_release_ns > 0
                        && monotonic_ns() < s->next_release_ns) {
                    struct itimerspec its{};
                    its.it_value.tv_sec  = s->next_release_ns / 1'000'000'000ULL;
                    its.it_value.tv_nsec = s->next_release_ns % 1'000'000'000ULL;
                    timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
                    // do not execute yet — wait for timerfd_ to fire
                } else {
                    should_execute = true;
                }
            }

            // timerfd fired: release time has arrived
            if (got_timer) should_execute = true;

            if (should_execute) {
                // Advance next_release_ns for strict periodicity
                if (s->period_ns > 0) {
                    uint64_t now = monotonic_ns();
                    s->next_release_ns = (s->next_release_ns == 0)
                        ? now + s->period_ns
                        : s->next_release_ns + s->period_ns;
                }

                s->execute();

                // Propagate to all downstream subtasks
                for (auto& conn : s->downstream)
                    conn.dispatcher->notify(conn.subtask);
            }
        }

        std::cerr << "[PreemptiveDispatcher] core=" << core_
                  << " subtask=" << subtask_->id << " stopped\n";
    }

    static void* static_loop(void* arg) {
        static_cast<PreemptiveDispatcher*>(arg)->loop();
        return nullptr;
    }

    Subtask*          subtask_;
    int               core_;
    int               priority_;
    int               efd_;
    int               timerfd_;
    int               epfd_;
    std::atomic<bool> running_;
    pthread_t         thread_;
};
