#include <iostream>
#include <unistd.h>
#include <sys/socket.h>

#include "ring_buffer.hpp"
#include "host_manager.hpp"

// ======================================================
//  socketpair() creates two connected file descriptors.
//  Writing to sockets[0] can be read from sockets[1]
//  and vice-versa — exactly like TCP, but without a real
//  network.
//
//  sockets[0] → Host A writes (Supplier side)
//  sockets[1] → Host B reads (Consumer side)
// ======================================================

int main() {

    RingBuffer<double, 4> ring;
    size_t seq = 0;

    int sockets[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    int supplier_socket = sockets[0];
    int consumer_socket = sockets[1];

    // -------------------------------------------------------
    //  Host B — Consumer side
    // -------------------------------------------------------
    HostManager host_b("Host-B");

    TeamManager*       team_b     = host_b.add_team("task_b");
    Demultiplexer*     d_consumer = team_b->add_demultiplexer(1, 49);
    SubtaskDescriptor* consumer   = team_b->add_subtask("Consumer", d_consumer, [&]() {
        double result = ring.read(seq) * 2.0;
        std::cout << "         [Consumer Host-B core 1] job " << seq
                  << "  result=" << result << "\n";
        ring.release(seq);
        seq++;
    });

    host_b.register_socket(consumer_socket, [&]() {
        double value;
        read(consumer_socket, &value, sizeof(value));
        ring.write(seq, value);
        std::cout << "  [Host-B demux] received job " << seq
                  << "  value=" << value << "\n";
        team_b->trigger(consumer);
    });

    host_b.start();

    // -------------------------------------------------------
    //  Host A — Supplier side
    // -------------------------------------------------------
    HostManager host_a("Host-A");

    TeamManager*       team_a     = host_a.add_team("task_a");
    Demultiplexer*     d_supplier = team_a->add_demultiplexer(0, 50);
    SubtaskDescriptor* supplier   = team_a->add_subtask("Supplier", d_supplier, [&]() {
        double value = seq * 10.0;
        std::cout << "  [Supplier Host-A core 0] job " << seq
                  << "  value=" << value << "  → sending\n";
        ::write(supplier_socket, &value, sizeof(value));
    });

    host_a.start();

    // -------------------------------------------------------
    //  Trigger 5 jobs from Host A
    // -------------------------------------------------------
    for (int i = 0; i < 5; i++) {
        sleep(1);
        team_a->trigger(supplier);
    }

    sleep(1);
    host_a.stop();
    host_b.stop();

    close(supplier_socket);
    close(consumer_socket);

    return 0;
}
