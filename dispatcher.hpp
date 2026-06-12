#pragma once

/**
 * @file dispatcher.hpp
 *
 * Implements the MCFlow dispatching subsystem (paper Section V-B, V-C).
 *
 * Key additions over the previous version
 * ----------------------------------------
 * - Subtask carries period_ns / next_release_ns for periodic scheduling.
 * - Subtask carries an atomic in_processing flag (required by leader/followers).
 * - Subtask carries fan_in_total / fan_in_received for multi-supplier fan-in.
 * - Subtask carries a downstream list so the dispatcher can automatically
 *   notify successors after execution (no manual wiring in execute()).
 * - Dispatcher owns a min-heap timer_queue_ for deferred periodic subtasks.
 * - Dispatcher owns an idle thread (SCHED_FIFO prio 1) that drains the timer
 *   queue when the CPU is otherwise idle.
 * - notify() enforces the fan-in condition before enqueuing.
 * - The 6-step release-guard protocol (Section V-C) is implemented in
 *   process_subtask(), which is also exposed via Demultiplexer::process().
 */

#include <queue>
#include <vector>
#include <functional>
#include <atomic>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>

class Dispatcher; // forward declaration for SubtaskConn

// -----------------------------------------------------------------------
//  Downstream connection descriptor
// -----------------------------------------------------------------------
struct SubtaskConn {
    Dispatcher*    dispatcher;
    struct Subtask* subtask;
};

// -----------------------------------------------------------------------
//  Subtask — unit of work with real-time scheduling metadata
// -----------------------------------------------------------------------
struct Subtask {
    int                   id;
    std::function<void()> execute;

    // Periodic scheduling: 0 = aperiodic (execute immediately every time)
    uint64_t period_ns       = 0;
    // Earliest absolute time (CLOCK_MONOTONIC ns) for next execution.
    // 0 = not yet initialised → execute immediately on first notification.
    uint64_t next_release_ns = 0;

    // Prevents concurrent execution when the leader/followers pattern is used.
    std::atomic<bool> in_processing{false};

    // Fan-in: how many upstream suppliers must notify before we dispatch.
    int              fan_in_total    = 1;  // default: single supplier
    std::atomic<int> fan_in_received{0};

    // Successors to notify automatically after this subtask finishes.
    std::vector<SubtaskConn> downstream;

    Subtask() = default;
    Subtask(int i, std::function<void()> fn)
        : id(i), execute(std::move(fn)) {}

    // Non-copyable: atomic members cannot be copied.
    Subtask(const Subtask&)            = delete;
    Subtask& operator=(const Subtask&) = delete;
};

// -----------------------------------------------------------------------
//  Timer queue support
// -----------------------------------------------------------------------
struct TimerEntry {
    uint64_t release_ns;
    Subtask* subtask;
    bool operator>(const TimerEntry& o) const { return release_ns > o.release_ns; }
};

using TimerQueue = std::priority_queue<TimerEntry,
                                       std::vector<TimerEntry>,
                                       std::greater<TimerEntry>>;

// -----------------------------------------------------------------------
//  Dispatcher
// -----------------------------------------------------------------------
class Dispatcher {
public:
    Dispatcher(int core, int priority)
        : core_(core), priority_(priority),
          efd_(-1), epfd_(-1), idle_efd_(-1), timerfd_(-1), running_(false) {}

    ~Dispatcher() { stop(); }

    void register_subtask(Subtask* s) { subtasks_.push_back(s); }

    /**
     * Signal that all preconditions for subtask s are met for one job.
     *
     * Implements fan-in: increments fan_in_received; enqueues s only when
     * all fan_in_total suppliers have signalled for this job.
     * Thread-safe; may be called from any thread.
     */
    void notify(Subtask* s) {
        int received = s->fan_in_received.fetch_add(1) + 1;
        if (received < s->fan_in_total) return;  // still waiting for more suppliers
        s->fan_in_received.store(0);              // reset for the next job

        pthread_mutex_lock(&queue_mutex_);
        queue_.push(s);
        pthread_mutex_unlock(&queue_mutex_);

        uint64_t sig = 1;
        ::write(efd_, &sig, sizeof(sig));
    }

    void start() {
        efd_      = eventfd(0, EFD_SEMAPHORE);
        epfd_     = epoll_create1(0);
        idle_efd_ = eventfd(0, EFD_SEMAPHORE);
        timerfd_  = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, efd_, &ev);

        pthread_mutex_init(&queue_mutex_, nullptr);
        pthread_mutex_init(&timer_mutex_, nullptr);

        running_ = true;
        pthread_create(&thread_,      nullptr, static_loop,      this);
        pthread_create(&idle_thread_, nullptr, static_idle_loop, this);
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        uint64_t wake = 1;
        ::write(efd_,      &wake, sizeof(wake));
        ::write(idle_efd_, &wake, sizeof(wake));

        pthread_join(thread_,      nullptr);
        pthread_join(idle_thread_, nullptr);

        close(efd_);      efd_      = -1;
        close(epfd_);     epfd_     = -1;
        close(idle_efd_); idle_efd_ = -1;
        close(timerfd_);  timerfd_  = -1;

        pthread_mutex_destroy(&queue_mutex_);
        pthread_mutex_destroy(&timer_mutex_);
    }

    // Returns current time in nanoseconds (CLOCK_MONOTONIC).
    static uint64_t monotonic_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // Exposed so Demultiplexer and the idle thread can call it.
    // Implements the 6-step release-guard protocol (paper Section V-C).
    void process_subtask(Subtask* s) {
        // Step 2: skip if already executing (leader/followers guard)
        if (s->in_processing.exchange(true)) return;

        uint64_t now = monotonic_ns();

        // Steps 3 & 4a: check if release time has arrived
        if (s->period_ns > 0 && s->next_release_ns > 0 && now < s->next_release_ns) {
            // Defer: push into timer queue; arm timerfd at the earliest release time
            s->in_processing.store(false);

            pthread_mutex_lock(&timer_mutex_);
            timer_queue_.push({s->next_release_ns, s});
            uint64_t earliest = timer_queue_.top().release_ns;
            pthread_mutex_unlock(&timer_mutex_);

            struct itimerspec its{};
            its.it_value.tv_sec  = earliest / 1'000'000'000ULL;
            its.it_value.tv_nsec = earliest % 1'000'000'000ULL;
            timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
            return;
        }

        // Step 4b: advance next_release_ns for strict periodicity
        if (s->period_ns > 0) {
            s->next_release_ns = (s->next_release_ns == 0)
                ? now + s->period_ns
                : s->next_release_ns + s->period_ns;
        }

        // Execute the subtask
        s->execute();

        // Step 5: propagate to downstream subtasks
        for (auto& conn : s->downstream)
            conn.dispatcher->notify(conn.subtask);

        // Step 6: clear in_processing
        s->in_processing.store(false);
    }

    // Called by the idle thread: dispatch earliest timer entry if past due.
    void dispatch_expired_timers() {
        uint64_t now = monotonic_ns();

        pthread_mutex_lock(&timer_mutex_);
        while (!timer_queue_.empty() && timer_queue_.top().release_ns <= now) {
            Subtask* s = timer_queue_.top().subtask;
            timer_queue_.pop();
            pthread_mutex_unlock(&timer_mutex_);

            pthread_mutex_lock(&queue_mutex_);
            queue_.push(s);
            pthread_mutex_unlock(&queue_mutex_);

            uint64_t sig = 1;
            ::write(efd_, &sig, sizeof(sig));

            pthread_mutex_lock(&timer_mutex_);
        }
        pthread_mutex_unlock(&timer_mutex_);
    }

private:
    void loop() {
        // Pin to designated core
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        // Apply real-time priority (requires CAP_SYS_NICE / root)
        struct sched_param param{};
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
            std::cerr << "[Dispatcher core=" << core_
                      << "] warning: RT priority not applied (run with sudo)\n";

        std::cerr << "[Dispatcher] core=" << core_
                  << " priority=" << priority_ << " started\n";

        struct epoll_event events[1];

        while (running_) {
            int n = epoll_wait(epfd_, events, 1, /*timeout_ms=*/20);
            if (n <= 0) continue;

            uint64_t val;
            ::read(efd_, &val, sizeof(val));

            // Step 1: dequeue one subtask
            pthread_mutex_lock(&queue_mutex_);
            if (queue_.empty()) { pthread_mutex_unlock(&queue_mutex_); continue; }
            Subtask* next = queue_.front();
            queue_.pop();
            pthread_mutex_unlock(&queue_mutex_);

            // Steps 2–6
            process_subtask(next);

            // Paper step 5 continuation: drain remaining ready subtasks
            while (true) {
                pthread_mutex_lock(&queue_mutex_);
                if (queue_.empty()) { pthread_mutex_unlock(&queue_mutex_); break; }
                next = queue_.front();
                queue_.pop();
                pthread_mutex_unlock(&queue_mutex_);
                process_subtask(next);
            }
        }

        std::cerr << "[Dispatcher] core=" << core_ << " stopped\n";
    }

    // Idle thread: lowest real-time priority — only runs when core is idle.
    // Drains the timer queue to allow early release (paper Section V-C).
    void idle_loop() {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        struct sched_param param{};
        param.sched_priority = 1; // minimum SCHED_FIFO priority
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        int idle_epfd = epoll_create1(0);
        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = idle_efd_;
        epoll_ctl(idle_epfd, EPOLL_CTL_ADD, idle_efd_, &ev);
        ev.data.fd = timerfd_;
        epoll_ctl(idle_epfd, EPOLL_CTL_ADD, timerfd_, &ev);

        struct epoll_event events[2];
        while (running_) {
            // timerfd fires at the exact next_release_ns; idle_efd_ handles shutdown.
            // 10ms fallback catches any edge-case races.
            int n = epoll_wait(idle_epfd, events, 2, /*timeout_ms=*/10);
            for (int i = 0; i < n; ++i) {
                uint64_t val;
                ::read(events[i].data.fd, &val, sizeof(val)); // drain token
            }

            dispatch_expired_timers();
        }

        close(idle_epfd);
    }

    static void* static_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->loop(); return nullptr;
    }
    static void* static_idle_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->idle_loop(); return nullptr;
    }

    int core_;
    int priority_;
    int efd_;
    int epfd_;
    int idle_efd_;
    int timerfd_;

    std::atomic<bool>    running_;
    std::queue<Subtask*> queue_;
    pthread_mutex_t      queue_mutex_;
    TimerQueue           timer_queue_;
    pthread_mutex_t      timer_mutex_;

    pthread_t thread_;
    pthread_t idle_thread_;

    std::vector<Subtask*> subtasks_;
};
