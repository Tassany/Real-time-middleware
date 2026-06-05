#include <iostream>
#include <unistd.h>

#include "ring_buffer.hpp"
#include "runtime.hpp"
#include "parser_json.hpp"

// ======================================================
//  Shared ring buffer between the four subtasks
// ======================================================

RingBuffer<double, 8> ring;
size_t seq = 0;

int main() {

    // --- Parse the deployment plan from JSON ---
    JsonParser    parser;
    DeploymentPlan plan = parser.parse("deployment_plan.json");

    // --- Create the Runtime for this host ---
    Runtime rt("server");

    // --- Register the execute function for each component ---
    //
    //  The names here must match "component" in the JSON.
    //  In a real system each function would read from its
    //  input ring buffer and write to its output ring buffer.
    //  Here we use a single shared buffer for simplicity.

    rt.register_component("Component1", [&]() {
        double value = seq * 10.0;
        ring.write(seq, value);
        std::cout << "  [T1 core 0] job " << seq
                  << "  produced=" << value << "\n";
    });

    rt.register_component("Component2", [&]() {
        double value = ring.read(seq) + 1.0;
        ring.write(seq, value);
        std::cout << "  [T2 core 1] job " << seq
                  << "  after +1  =" << value << "\n";
    });

    rt.register_component("Component3", [&]() {
        double value = ring.read(seq) * 2.0;
        ring.write(seq, value);
        std::cout << "  [T3 core 2] job " << seq
                  << "  after *2  =" << value << "\n";
    });

    rt.register_component("Component4", [&]() {
        double result = ring.read(seq);
        std::cout << "  [T4 core 3] job " << seq
                  << "  result   =" << result << "\n";
        ring.release(seq);
        seq++;
    });

    // --- Build the middleware from the plan ---
    //     Creates Dispatchers, Subtasks and wires connections
    std::cout << "\n=== Building from deployment plan ===\n";
    rt.build(plan);

    // --- Start all Dispatcher threads ---
    std::cout << "\n=== Starting ===\n";
    rt.start();

    // --- Trigger 4 jobs ---
    std::cout << "\n=== Running ===\n";
    for (int i = 0; i < 4; i++) {
        sleep(1);
        rt.trigger("T1");   // always triggers the first subtask in the chain
    }

    // --- Shutdown ---
    sleep(1);
    std::cout << "\n=== Stopping ===\n";
    rt.stop();

    return 0;
}
