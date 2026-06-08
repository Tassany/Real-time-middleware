#include <iostream>
#include <unistd.h>
#include "ring_buffer.hpp"
#include "dispatcher.hpp"

// ======================================================
//  Shared ring buffer between the two subtasks
// ======================================================

RingBuffer<double, 4> ring;
size_t current_seq = 0;

// ======================================================
//  Main
// ======================================================

int main() {

    // One Dispatcher per (core, priority) pair
    Dispatcher disp_supplier(0, 50);   // core 0, priority 50
    Dispatcher disp_consumer(1, 49);   // core 1, priority 49

    // --- Define the subtasks ---

    Subtask supplier;
    supplier.id = 1;

    Subtask consumer;
    consumer.id = 2;

    supplier.execute = [&]() {
        double value = current_seq * 10.0;
        ring.write(current_seq, value);

        std::cout << "  [Supplier core 0] job " << current_seq
                  << "  value=" << value << "\n";

        // Signal the Consumer Dispatcher that data is ready
        disp_consumer.notify(&consumer);
    };

    consumer.execute = [&]() {
        double& data   = ring.read(current_seq);
        double  result = data * 2.0;

        std::cout << "         [Consumer core 1] job " << current_seq
                  << "  result=" << result << "\n";

        ring.release(current_seq);
        current_seq++;
    };

    // --- Start the Dispatcher threads ---
    disp_supplier.start();
    disp_consumer.start();

    // --- Send 5 jobs to the Supplier ---
    for (int i = 0; i < 5; i++) {
        sleep(1);
        disp_supplier.notify(&supplier);
    }

    // --- Wait for jobs to finish then shut down ---
    sleep(1);
    disp_supplier.stop();
    disp_consumer.stop();

    return 0;
}
