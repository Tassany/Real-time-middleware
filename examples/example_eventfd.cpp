#include <iostream>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

int main() {

    // Create the eventfd — internal counter starts at 0
    int efd = eventfd(0, EFD_SEMAPHORE);

    // Supplier signals once
    uint64_t signal = 1; 
    write(efd, &signal, sizeof(signal));
    std::cout << "Supplier: signaled\n";

    // Consumer reads — receives 1, counter goes back to 0
    uint64_t received;
    read(efd, &received, sizeof(received));
    std::cout << "Consumer: received the signal\n";

    // Supplier signals 3 times
    write(efd, &signal, sizeof(signal));
    write(efd, &signal, sizeof(signal));
    write(efd, &signal, sizeof(signal));
    std::cout << "\nSupplier: signaled 3 times\n";

    // Consumer reads one by one (EFD_SEMAPHORE guarantees this)
    for (int i = 1; i <= 3; i++) {
        read(efd, &received, sizeof(received));
        std::cout << "Consumer: received signal " << i << "\n";
    }

    close(efd);
    return 0;
}
