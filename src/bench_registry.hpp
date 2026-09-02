#ifndef BENCH_REGISTRY_HPP
#define BENCH_REGISTRY_HPP

#include <string>

// ---------------------------------------------------------------------------
//  Mälardalen benchmarks as subtask bodies
//
//  Each wcet_bench/<name>.c is compiled with -Dmain=bench_entry_<name> and then
//  stripped down to that single global symbol with objcopy, because bsort100.c
//  and matmult.c both define Seed and Initialize and would otherwise collide at
//  link time. See the BENCH_* rules in the Makefile.
//
//  The wcet_ns values in the deployment plans come from wcet_bench/cycle_counter
//  (RELATORIO.md sec. 4) and are only valid for the flags recorded there:
//  gcc -std=gnu89 -O0 -fno-builtin -fno-stack-protector, on a Raspberry Pi 5
//  with the performance governor. Rebuilding these objects with a different -O
//  level silently invalidates every plan.
// ---------------------------------------------------------------------------
namespace bench {

using entry_fn = void (*)();

// Warm-up executions per benchmark before the real-time loop, matching the
// harness protocol (RELATORIO.md sec. 5.5: one warm-up is not enough; with five
// the monotonic decay along the batch disappears).
constexpr int WARMUP_RUNS = 5;

// Entry point for `name`, or nullptr if the name is not one of the six.
entry_fn lookup(const std::string& name);

// Comma-separated list of accepted names, for error messages.
std::string known_names();

// Runs the platform fix-ups and WARMUP_RUNS warm-up executions for `name`.
// Idempotent: the second call for the same name does nothing.
//
// Must run before the real-time loop, never inside it. Two of the six need it
// for correctness, not just for timing:
//   bsort100 reads the absolute address 0x80200001 and segfaults on Linux
//            unless that page is mapped first (RELATORIO.md sec. 5.6);
//   crc      builds a static 256-entry table on its first call only, which
//            costs about 20x a steady-state call (RELATORIO.md sec. 5.4), so
//            the declared 0.821 us only describes calls after the warm-up.
//
// Throws std::runtime_error on an unknown name or a failed fix-up.
//
// NOTE: the warm-up runs on the calling thread. That resolves page faults and
// the first-call state above, which are process-wide, but it does not warm the
// L1I and branch predictor of the core the subtask will later run on, since
// those are per-core. Cross-core micro-architectural warm-up is not addressed.
void prepare(const std::string& name);

} // namespace bench

#endif // BENCH_REGISTRY_HPP
