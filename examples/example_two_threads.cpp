#include <iostream>
#include <cstdint>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>

#include "ring_buffer.hpp"

// ======================================================
//  Pin the calling thread to the given core
// ======================================================

void pin_to_core(int core) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
}

// ======================================================
//  Set real-time SCHED_FIFO priority for the calling thread
//  Requires sudo to take effect.
// ======================================================

void set_priority(int priority) {
    struct sched_param param;
    param.sched_priority = priority;

    int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (result != 0)
        std::cout << "  [warning] RT priority not applied — run with sudo\n";
}

// ======================================================
//  Shared context between Supplier and Consumer
// ======================================================

struct Context {
    RingBuffer<double, 4> ring;
    int efd;
};

// ======================================================
//  Supplier thread — core 0, priority 50
// ======================================================

void* supplier(void* arg) {
    pin_to_core(0);
    set_priority(50);

    Context* ctx = static_cast<Context*>(arg);

    for (size_t seq = 0; seq < 5; seq++) {

        double value = seq * 10.0;
        ctx->ring.write(seq, value);

        std::cout << "[Supplier core 0 prio 50] job " << seq
                  << "  value=" << value << "\n";

        uint64_t signal = 1;
        write(ctx->efd, &signal, sizeof(signal));

        sleep(1);
    }

    return nullptr;
}

// ======================================================
//  Consumer thread — core 1, priority 49
// ======================================================

void* consumer(void* arg) {
    pin_to_core(1);
    set_priority(49);

    Context* ctx = static_cast<Context*>(arg);

    for (size_t seq = 0; seq < 5; seq++) {

        // Sleep here until the Supplier signals
        uint64_t received;
        read(ctx->efd, &received, sizeof(received));

        double& data   = ctx->ring.read(seq);
        double  result = data * 2.0;

        std::cout << "         [Consumer core 1 prio 49] job " << seq
                  << "  result=" << result << "\n";

        ctx->ring.release(seq);
    }

    return nullptr;
}

// ======================================================
//  Main
// ======================================================

int main() {

    Context ctx;
    ctx.efd = eventfd(0, EFD_SEMAPHORE);

    pthread_t t_supplier, t_consumer;
    pthread_create(&t_supplier, nullptr, supplier, &ctx);
    pthread_create(&t_consumer, nullptr, consumer, &ctx);

    pthread_join(t_supplier, nullptr);
    pthread_join(t_consumer, nullptr);

    close(ctx.efd);
    return 0;
}
