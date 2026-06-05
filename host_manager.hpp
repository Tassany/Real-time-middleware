#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <functional>
#include <stdexcept>

#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "team_manager.hpp"

// ================================================================
//  SocketHandler
//
//  Associates a file descriptor (socket or eventfd) with a callback.
//  When epoll reports activity on the fd, the callback is invoked.
// ================================================================

struct SocketHandler {
    int                   fd;
    std::function<void()> on_data;   // called when fd has data to read
};

// ================================================================
//  HostManager
//
//  Top-level component for one host in the MCFlow system.
//
//  Responsibilities:
//  - Owns all TeamManagers running on this host
//  - Runs a network demultiplexer loop (epoll) that monitors
//    incoming sockets and routes data to the right TeamManager
//  - Coordinates startup and shutdown across all TeamManagers
//
//  The demultiplexer loop runs in its own thread so that network
//  I/O never blocks the real-time Dispatcher threads.
// ================================================================

class HostManager {
public:

    explicit HostManager(const std::string& name)
        : name_(name), running_(false) {}

    ~HostManager() {
        if (running_) stop();
        for (auto* tm : teams_) delete tm;
    }

    // -------------------------------------------------------
    //  add_team()
    //
    //  Creates a new TeamManager and registers it with this host.
    //  Returns a pointer so you can configure its subtasks.
    // -------------------------------------------------------
    TeamManager* add_team(const std::string& task_name) {
        auto* tm = new TeamManager(task_name);
        teams_.push_back(tm);
        return tm;
    }

    // -------------------------------------------------------
    //  register_socket(fd, callback)
    //
    //  Registers a file descriptor to be monitored by epoll.
    //  When data arrives on fd, callback() is invoked from
    //  the demultiplexer thread.
    //
    //  Use this to connect incoming network sockets to the
    //  ring buffer and Dispatcher of the target Consumer.
    // -------------------------------------------------------
    void register_socket(int fd, std::function<void()> callback) {
        handlers_.push_back({fd, callback});
    }

    // -------------------------------------------------------
    //  start()
    //
    //  Starts all TeamManagers, then launches the
    //  demultiplexer thread to handle network events.
    // -------------------------------------------------------
    void start() {
        std::cout << "[HostManager] " << name_ << " starting\n";

        // Start all teams
        for (auto* tm : teams_)
            tm->start();

        // Create epoll and register all handlers
        epfd_     = epoll_create1(0);
        stop_efd_ = eventfd(0, EFD_SEMAPHORE);

        // Register the stop signal
        add_to_epoll(stop_efd_);

        // Register all user-provided sockets
        for (auto& h : handlers_)
            add_to_epoll(h.fd);

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);

        std::cout << "[HostManager] " << name_ << " running\n";
    }

    // -------------------------------------------------------
    //  stop()
    //
    //  Stops the demultiplexer thread, then stops all teams.
    // -------------------------------------------------------
    void stop() {
        if (!running_) return;
        std::cout << "[HostManager] " << name_ << " stopping\n";

        running_ = false;

        // Wake up the demultiplexer loop
        uint64_t sig = 1;
        write(stop_efd_, &sig, sizeof(sig));

        pthread_join(thread_, nullptr);

        for (auto* tm : teams_)
            tm->stop();

        close(epfd_);
        close(stop_efd_);

        std::cout << "[HostManager] " << name_ << " terminated\n";
    }

private:

    // -------------------------------------------------------
    //  Demultiplexer loop
    //
    //  Monitors all registered file descriptors.
    //  When a fd has data, finds the matching handler and
    //  calls its callback — which routes data to the Consumer.
    // -------------------------------------------------------
    void loop() {
        std::cout << "[HostManager] demux thread started\n";

        struct epoll_event events[16];

        while (running_) {
            int n = epoll_wait(epfd_, events, 16, /*timeout ms=*/100);

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;

                // Stop signal
                if (fd == stop_efd_) break;

                // Find the handler for this fd and invoke it
                for (auto& h : handlers_) {
                    if (h.fd == fd) {
                        h.on_data();
                        break;
                    }
                }
            }
        }

        std::cout << "[HostManager] demux thread stopped\n";
    }

    void add_to_epoll(int fd) {
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    static void* static_loop(void* arg) {
        static_cast<HostManager*>(arg)->loop();
        return nullptr;
    }

    std::string               name_;
    bool                      running_;
    int                       epfd_;
    int                       stop_efd_;
    pthread_t                 thread_;

    std::vector<TeamManager*>  teams_;
    std::vector<SocketHandler> handlers_;
};
