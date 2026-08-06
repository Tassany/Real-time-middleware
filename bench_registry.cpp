#include "bench_registry.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
//  Entry points
//
//  The six sources do not agree on a signature: matmult.c, ud.c and fft1.c
//  declare `void main()`, crc.c and statemate.c declare `int main(void)`, and
//  bsort100.c uses the K&R `main()` with an implicit int. Each one is therefore
//  declared with its own return type and wrapped in a uniform void() thunk;
//  calling a void-returning function through an int(*)() pointer would be
//  undefined behaviour.
// ---------------------------------------------------------------------------
extern "C" {
void bench_entry_matmult(void);
void bench_entry_ud(void);
void bench_entry_fft1(void);
int  bench_entry_crc(void);
int  bench_entry_statemate(void);
int  bench_entry_bsort100(void);
}

namespace {

void call_matmult()   { bench_entry_matmult(); }
void call_ud()        { bench_entry_ud(); }
void call_fft1()      { bench_entry_fft1(); }
void call_crc()       { (void)bench_entry_crc(); }
void call_statemate() { (void)bench_entry_statemate(); }
void call_bsort100()  { (void)bench_entry_bsort100(); }

const std::map<std::string, bench::entry_fn>& table() {
    static const std::map<std::string, bench::entry_fn> t = {
        { "matmult",   &call_matmult   },
        { "bsort100",  &call_bsort100  },
        { "crc",       &call_crc       },
        { "ud",        &call_ud        },
        { "fft1",      &call_fft1      },
        { "statemate", &call_statemate },
    };
    return t;
}

// MAP_FIXED_NOREPLACE exists since Linux 4.17. On an older libc fall back to
// MAP_FIXED, which does the same thing but silently overwrites an existing
// mapping instead of failing.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

// bsort100.c opens with
//     #define KNOWN_VALUE (int)(*((char *)0x80200001))
// and reads that absolute address expecting to find 1, which worked on the
// bare-metal target it was written for. Map the page instead of patching the
// source, so the benchmark stays byte-identical to the one that was measured.
void map_bsort100_page() {
    void*  base = reinterpret_cast<void*>(0x80200000UL);
    size_t len  = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    void* p = mmap(base, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (p == MAP_FAILED || p != base)
        throw std::runtime_error(
            "bench: could not map the page at 0x80200000 that bsort100 reads");

    auto* b = static_cast<unsigned char*>(p);
    b[1] = 1;   // KNOWN_VALUE, the only one bsort100 uses
    b[3] = 1;   // UNKNOWN_VALUE, defined in the source but never called
}

} // namespace

namespace bench {

entry_fn lookup(const std::string& name) {
    auto it = table().find(name);
    return (it == table().end()) ? nullptr : it->second;
}

std::string known_names() {
    std::string s;
    for (const auto& [name, fn] : table()) {
        (void)fn;
        s += (s.empty() ? "" : ", ") + name;
    }
    return s;
}

void prepare(const std::string& name) {
    static std::set<std::string> done;
    if (done.count(name)) return;

    entry_fn fn = lookup(name);
    if (!fn)
        throw std::runtime_error("subtask benchmark: unknown value '" + name +
                                 "' (expected " + known_names() + ")");

    if (name == "bsort100") map_bsort100_page();

    for (int i = 0; i < WARMUP_RUNS; ++i) fn();

    done.insert(name);
}

} // namespace bench
