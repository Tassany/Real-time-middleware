#pragma once

#include <functional>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <atomic>
#include <iostream>

#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>

// ================================================================
//  SubtaskDescriptor
//
//  Everything the Demultiplexer needs to know about one subtask:
//  how to run it, whether it is periodic, and its release guard state.
// ================================================================

struct SubtaskDescriptor {
    std::string           name;
    std::function<void()> execute;

    // Release guard (§V-C of the paper)
    bool     is_periodic  = false;
    long     period_ns    = 0;       // period in nanoseconds
    timespec next_release = {0, 0};  // absolute time of the next allowed release
                                     // initialised to clock_gettime in start()

    // In-processing flag: prevents two threads (leader/followers)
    // from executing the same subtask concurrently
    std::atomic<bool> in_processing{false};

    // Fan-in (§V-B, Figure 7): consumer is dispatched only when all
    // upstream suppliers have delivered data for the current job.
    // supplier_count is set by TeamManager::add_connection().
    // pending_suppliers is initialised to supplier_count in start()
    // and reset after each complete fan-in cycle.
    int              supplier_count = 0;
    std::atomic<int> pending_suppliers{0};

    // Wired by the Runtime to ComponentBase::has_pending_input().
    // Used by the Demultiplexer Step 5 drain (§V-C): keep executing
    // while there are unconsumed entries in the input_port ring buffer.
    // Left empty for lambda-only subtasks (no ComponentPort).
    std::function<bool()> has_pending_input;

    // eventfd that the upstream Supplier writes to when data is ready
    int efd = -1;
};

// ================================================================
//  TimerEntry
//
//  A subtask waiting in the timer queue for its release time.
// ================================================================

struct TimerEntry {
    timespec         release_time;
    SubtaskDescriptor* subtask;

    // Min-heap: earliest release time first
    bool operator>(const TimerEntry& other) const {
        if (release_time.tv_sec != other.release_time.tv_sec)
            return release_time.tv_sec > other.release_time.tv_sec;
        return release_time.tv_nsec > other.release_time.tv_nsec;
    }
};

// ================================================================
//  EpollKind / EpollEntry  (fix 1.1)
//
//  epoll_event.data is a union — only one field is valid per registration.
//  Using data.ptr uniformly for all fds eliminates UB from mixing
//  data.fd and data.ptr reads in the same loop iteration.
// ================================================================

enum class EpollKind { STOP, TIMER, SUBTASK };

struct EpollEntry {
    EpollKind          kind;
    SubtaskDescriptor* desc = nullptr;  // valid only when kind == SUBTASK
};

// ================================================================
//  Demultiplexer
//
//  Sits between event sources (eventfd / network sockets) and
//  the actual subtask execution.
//
//  Architecture (§V-C of the paper):
//  - Monitors all registered eventfds via epoll
//  - For each event, checks whether the subtask can run:
//      • not currently in-processing (leader/followers safety)
//      • release time has passed (release guard)
//  - If release time is in the future: inserts into timer queue
//  - An idle thread executes subtasks early when the CPU is free
//
//  One Demultiplexer is created per Dispatcher (per core/priority).
// ================================================================

class Demultiplexer {
public:

    Demultiplexer(int core, int priority)
        : core_(core), priority_(priority), running_(false) {}

    ~Demultiplexer() { stop(); }

    // -------------------------------------------------------
    //  register_subtask(desc)
    //
    //  Registers a subtask and its eventfd with the epoll loop.
    //  The eventfd is created here and returned so the upstream
    //  Supplier can write to it when data is ready.
    // -------------------------------------------------------
    int register_subtask(SubtaskDescriptor* desc) {
        desc->efd = eventfd(0, EFD_SEMAPHORE);

        // EpollEntry is allocated in epoll_entries_ below in start().
        // Store the descriptor pointer now; the entry is built in start().
        descriptors_.push_back(desc);
        return desc->efd;
    }

    // -------------------------------------------------------
    //  start() — launches the demux thread
    // -------------------------------------------------------
    void start() {
        epfd_     = epoll_create1(0);
        stop_efd_ = eventfd(0, EFD_SEMAPHORE);
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

        // Build EpollEntry objects.  They must live for the full lifetime of
        // the epoll instance because epoll_event.data.ptr points into them.
        // Reserve up front to prevent reallocation (which would invalidate ptrs).
        epoll_entries_.reserve(descriptors_.size() + 2);

        // STOP entry
        epoll_entries_.push_back({EpollKind::STOP, nullptr});
        register_epoll_entry(stop_efd_, epoll_entries_.back());

        // TIMER entry
        epoll_entries_.push_back({EpollKind::TIMER, nullptr});
        register_epoll_entry(timer_fd_, epoll_entries_.back());

        // SUBTASK entries — one per registered descriptor
        for (auto* desc : descriptors_) {
            epoll_entries_.push_back({EpollKind::SUBTASK, desc});
            register_epoll_entry(desc->efd, epoll_entries_.back());
        }

        pthread_mutex_init(&timer_queue_mutex_, nullptr);

        // Fix 1.2: initialise next_release for periodic subtasks to the
        // current monotonic time so the release guard computes correct future
        // deadlines instead of comparing against the Unix epoch {0, 0}.
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (auto* desc : descriptors_) {
            if (desc->is_periodic && desc->next_release.tv_sec == 0)
                desc->next_release = now;
        }

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);
    }

    // -------------------------------------------------------
    //  stop()
    // -------------------------------------------------------
    void stop() {
        if (!running_) return;
        running_ = false;

        uint64_t sig = 1;
        write(stop_efd_, &sig, sizeof(sig));

        pthread_join(thread_, nullptr);

        for (auto* desc : descriptors_)
            if (desc->efd >= 0) close(desc->efd);

        close(epfd_);
        close(stop_efd_);
        close(timer_fd_);
        pthread_mutex_destroy(&timer_queue_mutex_);
    }

private:

    // -------------------------------------------------------
    //  register_epoll_entry — helper used in start()
    // -------------------------------------------------------
    void register_epoll_entry(int fd, EpollEntry& entry) {
        struct epoll_event ev;
        ev.events   = EPOLLIN;
        ev.data.ptr = &entry;   // uniform use of data.ptr (fix 1.1)
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    // -------------------------------------------------------
    //  Main loop
    // -------------------------------------------------------
    void loop() {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        struct sched_param param;
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
            std::cout << "  [warning] RT priority not applied, run with sudo\n";

        std::cout << "[Demux] core=" << core_
                  << " priority=" << priority_ << " started\n";

        struct epoll_event events[16];

        while (running_) {
            int n = epoll_wait(epfd_, events, 16, /*timeout ms=*/100);

            for (int i = 0; i < n; i++) {
                // All fds are registered with data.ptr — safe to cast directly.
                EpollEntry* entry = static_cast<EpollEntry*>(events[i].data.ptr);

                switch (entry->kind) {

                case EpollKind::STOP:
                    running_ = false;
                    break;

                case EpollKind::TIMER: {
                    uint64_t expirations;
                    read(timer_fd_, &expirations, sizeof(expirations));
                    dispatch_from_timer_queue();
                    break;
                }

                case EpollKind::SUBTASK: {
                    SubtaskDescriptor* desc = entry->desc;
                    uint64_t val;
                    read(desc->efd, &val, sizeof(val));
                    process_notification(desc);
                    break;
                }
                }

                if (!running_) break;
            }
        }

        std::cout << "[Demux] core=" << core_ << " stopped\n";
    }

    // -------------------------------------------------------
    //  process_notification (§V-C steps 1–6)
    // -------------------------------------------------------
    void process_notification(SubtaskDescriptor* desc) {

        // Step 2: in-processing check (leader/followers safety)
        bool expected = false;
        if (!desc->in_processing.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            requeue(desc);
            return;
        }

        // Step 3: release guard
        if (desc->is_periodic) {
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            if (!time_reached(now, desc->next_release)) {
                desc->in_processing.store(false, std::memory_order_release);
                insert_timer_queue(desc);
                return;
            }

            advance_release_time(desc);
        }

        // Steps 4 + 5: execute and drain pending inputs (§V-C)
        std::cout << "[Demux core " << core_ << "] executing: "
                  << desc->name << "\n";

        do {
            desc->execute();
            // Step 5: keep executing while the input_port still has data.
            // has_pending_input is wired to ComponentBase::has_pending_input()
            // by the Runtime. Empty for lambda-only subtasks → loop runs once.
        } while (desc->has_pending_input && desc->has_pending_input());

        // Step 6: clear in-processing flag
        desc->in_processing.store(false, std::memory_order_release);
    }

    // -------------------------------------------------------
    //  Timer queue helpers
    // -------------------------------------------------------
    void insert_timer_queue(SubtaskDescriptor* desc) {
        pthread_mutex_lock(&timer_queue_mutex_);
        timer_queue_.push({desc->next_release, desc});
        pthread_mutex_unlock(&timer_queue_mutex_);
        arm_timerfd();
    }

    void dispatch_from_timer_queue() {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        pthread_mutex_lock(&timer_queue_mutex_);
        while (!timer_queue_.empty()) {
            const TimerEntry& top = timer_queue_.top();
            if (!time_reached(now, top.release_time)) break;

            SubtaskDescriptor* desc = top.subtask;
            timer_queue_.pop();
            pthread_mutex_unlock(&timer_queue_mutex_);

            process_notification(desc);

            pthread_mutex_lock(&timer_queue_mutex_);
        }
        pthread_mutex_unlock(&timer_queue_mutex_);

        if (!timer_queue_.empty())
            arm_timerfd();
    }

    void arm_timerfd() {
        pthread_mutex_lock(&timer_queue_mutex_);
        if (timer_queue_.empty()) {
            pthread_mutex_unlock(&timer_queue_mutex_);
            return;
        }
        timespec next = timer_queue_.top().release_time;
        pthread_mutex_unlock(&timer_queue_mutex_);

        struct itimerspec spec = {};
        spec.it_value = next;
        timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr);
    }

    void requeue(SubtaskDescriptor* desc) {
        uint64_t sig = 1;
        write(desc->efd, &sig, sizeof(sig));
    }

    // -------------------------------------------------------
    //  Time utilities
    // -------------------------------------------------------
    static bool time_reached(const timespec& now, const timespec& target) {
        if (now.tv_sec  != target.tv_sec)
            return now.tv_sec  > target.tv_sec;
        return now.tv_nsec >= target.tv_nsec;
    }

    static void advance_release_time(SubtaskDescriptor* desc) {
        desc->next_release.tv_nsec += desc->period_ns;
        while (desc->next_release.tv_nsec >= 1'000'000'000L) {
            desc->next_release.tv_nsec -= 1'000'000'000L;
            desc->next_release.tv_sec++;
        }
    }

    static void* static_loop(void* arg) {
        static_cast<Demultiplexer*>(arg)->loop();
        return nullptr;
    }

    // -------------------------------------------------------
    //  Internal state
    // -------------------------------------------------------
    int core_;
    int priority_;
    int epfd_;
    int stop_efd_;
    int timer_fd_;

    std::atomic<bool> running_;
    pthread_t         thread_;

    std::vector<SubtaskDescriptor*> descriptors_;

    // EpollEntry objects must stay alive as long as epfd_ is open because
    // epoll_event.data.ptr points directly into this vector.
    // reserve() in start() prevents reallocation after pointers are stored.
    std::vector<EpollEntry> epoll_entries_;

    std::priority_queue<TimerEntry,
                        std::vector<TimerEntry>,
                        std::greater<TimerEntry>> timer_queue_;
    pthread_mutex_t timer_queue_mutex_;
};
