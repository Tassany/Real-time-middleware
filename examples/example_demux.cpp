#include <iostream>
#include <unistd.h>
#include <cstdint>
#include <time.h>
#include "demultiplexer.hpp"

// ======================================================
//  This example shows the Release Guard in action.
//
//  A Supplier signals the eventfd every 200ms (5 Hz),
//  but the subtask is periodic with a 500ms period (2 Hz).
//
//  Without the release guard: subtask would execute 5x/sec
//  With the release guard:    subtask executes at most 2x/sec
// ======================================================

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1'000'000;
}

int main() {

    // --- Create the subtask descriptor ---
    SubtaskDescriptor desc;
    desc.name    = "PeriodicSubtask";
    desc.execute = [&]() {
        std::cout << "  [subtask] executed at t=" << now_ms() << " ms\n";
    };

    // Periodic: 500ms period, release guard enforced
    desc.is_periodic = true;
    desc.period_ns   = 500'000'000L;   // 500 ms in nanoseconds

    // Set first release time to now
    clock_gettime(CLOCK_MONOTONIC, &desc.next_release);

    // --- Create and configure the Demultiplexer ---
    Demultiplexer demux(0, 50);
    int efd = demux.register_subtask(&desc);
    demux.start();

    // --- Supplier: signals every 200ms (faster than the period) ---
    std::cout << "Supplier signaling every 200ms, subtask period=500ms\n\n";

    long start = now_ms();

    for (int i = 0; i < 15; i++) {
        usleep(200'000);   // 200ms

        uint64_t sig = 1;
        write(efd, &sig, sizeof(sig));
        std::cout << "[supplier] signaled at t=" << (now_ms() - start) << " ms\n";
    }

    sleep(1);
    demux.stop();

    return 0;
}
