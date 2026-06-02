#pragma once

/**
 * @file dispatcher.hpp
 * @brief Real-time task dispatcher pinned to a specific CPU core.
 *
 * Architecture overview
 * ---------------------
 * Each Dispatcher owns a single POSIX thread pinned to one CPU core via
 * pthread_setaffinity_np().  The thread runs at a real-time SCHED_FIFO
 * priority and blocks on epoll_wait() when idle, consuming no CPU time.
 *
 * Wakeup mechanism
 * ----------------
 * An eventfd(EFD_SEMAPHORE) acts as a lightweight notification channel.
 * When notify() is called (from any thread), it:
 *   1. Pushes the Subtask pointer onto a mutex-protected FIFO queue.
 *   2. Writes 1 to the eventfd, which increments its counter and wakes epoll.
 *
 * The dispatcher thread drains one entry per wakeup:
 *   epoll_wait -> read(eventfd) -> dequeue subtask -> subtask.execute()
 *
 * This design avoids busy-waiting and keeps latency low while remaining
 * compatible with standard Linux real-time scheduling.
 */

#include <queue>
#include <vector>
#include <functional>
#include <atomic>
#include <string>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>


/**
 * @brief A named, executable unit of work corresponding to one DAG node.
 *
 * The execute functor can wrap any callable — a free function, a lambda,
 * or a bound member function — making Subtask decoupled from Component.
 */
struct Subtask {
    std::string           name;    ///< Human-readable label for logging.
    std::function<void()> execute; ///< The actual computation to run.
};


/**
 * @brief Executes subtasks on a dedicated real-time thread pinned to one CPU core.
 *
 * One Dispatcher is created per (core, priority) pair in the deployment plan.
 * After calling start(), subtasks are submitted via notify() from any thread.
 * Call stop() (or let the destructor do it) to drain the queue and join the thread.
 *
 * @note Requires CAP_SYS_NICE or root privileges to apply SCHED_FIFO priority.
 *       Without them the thread still runs, but without real-time guarantees.
 */
class Dispatcher {
public:

    /**
     * @param core     CPU core index (0-based) to pin the thread to.
     * @param priority SCHED_FIFO real-time priority (1 = lowest, 99 = highest).
     */
    Dispatcher(int core, int priority)
        : core_(core), priority_(priority), running_(false) {}

    /** @brief Calls stop() if the Dispatcher is still running. */
    ~Dispatcher() { stop(); }

    /**
     * @brief Register a subtask as eligible to run on this Dispatcher.
     *
     * Registration is informational at this stage; the Dispatcher does not
     * schedule subtasks autonomously — callers trigger execution via notify().
     *
     * @param s Pointer to a Subtask. Must outlive the Dispatcher.
     */
    void register_subtask(Subtask* s) {
        subtasks_.push_back(s);
    }

    /**
     * @brief Signal that subtask @p s is ready to execute.
     *
     * Thread-safe; may be called from any thread, including other Dispatchers.
     * The subtask is queued and the internal thread is woken via eventfd.
     *
     * @param s Subtask to enqueue. Must not be nullptr.
     */
    void notify(Subtask* s) {
        pthread_mutex_lock(&queue_mutex_);
        queue_.push(s);
        pthread_mutex_unlock(&queue_mutex_);

        // Increment the eventfd semaphore to wake up epoll_wait in loop().
        uint64_t signal = 1;
        write(efd_, &signal, sizeof(signal));
    }

    /**
     * @brief Create the eventfd/epoll descriptors and launch the worker thread.
     *
     * Must be called exactly once before any notify() calls.
     */
    void start() {
        efd_  = eventfd(0, EFD_SEMAPHORE);
        epfd_ = epoll_create1(0);

        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, efd_, &ev);

        pthread_mutex_init(&queue_mutex_, nullptr);

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);
    }

    /**
     * @brief Signal the worker thread to stop and block until it exits.
     *
     * Idempotent — safe to call multiple times or when never started.
     * Closes the eventfd and epoll file descriptors after joining.
     */
    void stop() {
        if (!running_) return;
        running_ = false;

        // Write to the eventfd so epoll_wait returns and the loop sees !running_.
        uint64_t wake = 1;
        write(efd_, &wake, sizeof(wake));

        pthread_join(thread_, nullptr);
        close(efd_);
        close(epfd_);
        pthread_mutex_destroy(&queue_mutex_);
    }

private:

    /**
     * @brief Main event loop executed inside the worker thread.
     *
     * Steps per iteration:
     *   1. epoll_wait() — sleep until a notification arrives (100 ms timeout
     *      so the loop can detect running_ == false even with no events).
     *   2. read(eventfd) — consume one semaphore count.
     *   3. Dequeue the next Subtask under the mutex.
     *   4. Call subtask->execute() (no lock held during execution).
     */
    void loop() {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        struct sched_param param;
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
            std::cout << "  [warning] RT priority not applied, run with sudo\n";

        std::cout << "[Dispatcher] core=" << core_
                  << " priority=" << priority_ << " started\n";

        struct epoll_event events[1];

        while (running_) {
            int n = epoll_wait(epfd_, events, 1, /*timeout_ms=*/100);
            if (n <= 0) continue;

            uint64_t val;
            read(efd_, &val, sizeof(val));

            pthread_mutex_lock(&queue_mutex_);
            if (queue_.empty()) {
                pthread_mutex_unlock(&queue_mutex_);
                continue;
            }
            Subtask* next = queue_.front();
            queue_.pop();
            pthread_mutex_unlock(&queue_mutex_);

            std::cout << "[Dispatcher core " << core_ << "] running: "
                      << next->name << "\n";
            next->execute();
        }

        std::cout << "[Dispatcher] core=" << core_ << " stopped\n";
    }

    /** pthread_create requires a static function; this trampoline forwards to loop(). */
    static void* static_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->loop();
        return nullptr;
    }

    int core_;      ///< CPU core index for thread affinity.
    int priority_;  ///< SCHED_FIFO real-time priority.
    int efd_;       ///< eventfd file descriptor (semaphore mode).
    int epfd_;      ///< epoll instance that watches efd_.

    std::atomic<bool>    running_;     ///< Loop exit flag; set to false by stop().
    std::queue<Subtask*> queue_;       ///< FIFO of subtasks waiting to execute.
    pthread_mutex_t      queue_mutex_; ///< Protects queue_ during push/pop.
    pthread_t            thread_;      ///< The worker thread handle.

    std::vector<Subtask*> subtasks_;   ///< All subtasks registered to this Dispatcher.
};
