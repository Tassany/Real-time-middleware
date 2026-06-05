#include <iostream>
#include <unistd.h>
#include "ring_buffer.hpp"
#include "team_manager.hpp"

int main() {

    RingBuffer<double, 4> ring;
    size_t seq = 0;

    // -------------------------------------------------------
    //  Build the team using Demultiplexer (Phase 2 API)
    // -------------------------------------------------------
    TeamManager tm("sensor_task");

    Demultiplexer* d0 = tm.add_demultiplexer(0, 50);  // core 0, priority 50
    Demultiplexer* d1 = tm.add_demultiplexer(1, 49);  // core 1, priority 49

    SubtaskDescriptor* supplier = tm.add_subtask("Supplier", d0, [&]() {
        double value = seq * 10.0;
        ring.write(seq, value);
        std::cout << "  [Supplier core 0] job " << seq
                  << "  value=" << value << "\n";
    });

    SubtaskDescriptor* consumer = tm.add_subtask("Consumer", d1, [&]() {
        double result = ring.read(seq) * 2.0;
        std::cout << "         [Consumer core 1] job " << seq
                  << "  result=" << result << "\n";
        ring.release(seq);
        seq++;
    });

    // Wire: when Supplier finishes → notify Consumer's Demultiplexer
    tm.add_connection(supplier, consumer);

    // -------------------------------------------------------
    //  Run
    // -------------------------------------------------------
    tm.start();

    for (int i = 0; i < 5; i++) {
        sleep(1);
        tm.trigger(supplier);
    }

    sleep(1);
    tm.stop();

    return 0;
}
