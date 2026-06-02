#include <iostream>
#include <cstdint>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <pthread.h>

// ======================================================
//  This example shows epoll waiting on an eventfd.
//
//  Main thread: sleeps via epoll_wait
//  Second thread: signals the eventfd after 2 seconds
// ======================================================

int efd;   // eventfd shared between the two threads

// Second thread — signals after 2 seconds
void* signaler(void* arg) {
    sleep(2);

    std::cout << "[signaler] sending signal...\n";
    uint64_t signal = 1;
    write(efd, &signal, sizeof(signal));

    return nullptr;
}

int main() {

    // Step 1 — create the eventfd
    efd = eventfd(0, EFD_SEMAPHORE);

    // Step 2 — create the epoll
    int epfd = epoll_create1(0);

    // Step 3 — register the eventfd with epoll
    struct epoll_event ev;
    ev.events  = EPOLLIN;   // wake up when there is data to read
    ev.data.fd = efd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);

    // Launch the thread that will signal us
    pthread_t t;
    pthread_create(&t, nullptr, signaler, nullptr);

    std::cout << "[main] waiting for signal via epoll...\n";

    // Step 4 — sleep here until the eventfd is signaled
    struct epoll_event events[1];
    epoll_wait(epfd, events, 1, /*timeout ms=*/ -1);

    // Read the eventfd to consume the signal
    uint64_t received;
    read(efd, &received, sizeof(received));

    std::cout << "[main] woke up! signal received.\n";

    pthread_join(t, nullptr);
    close(efd);
    close(epfd);
    return 0;
}
