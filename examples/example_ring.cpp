#include <iostream>
#include "ring_buffer.hpp"

int main() {

    // Ring buffer with 4 slots, storing doubles
    RingBuffer<double, 4> ring;

    // Simulate 6 jobs in sequence (single thread, no parallelism)
    for (size_t seq = 0; seq < 6; seq++) {
                ring.release(1);

        // Supplier: compute and write
        double value = seq * 1.5;
        ring.write(seq, value);

        // Consumer: read and process
        double& data   = ring.read(seq);
        double  result = data * 2.0;

        std::cout << "job " << seq
                  << "  slot=" << (seq % 4)
                  << "  written=" << value
                  << "  read=" << result
                  << "\n";
    }

    return 0;
}
