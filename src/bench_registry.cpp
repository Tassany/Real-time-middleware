#include "bench_registry.hpp"

#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
//  Entry points
//
//  The 32 sources do not agree on a signature: some declare `void main()`,
//  others `int main(void)` (or a variant), and a few use the K&R `main()`
//  with an implicit int. Each one is therefore declared with its own return
//  type and wrapped in a uniform void() thunk; calling a void-returning
//  function through an int(*)() pointer would be undefined behaviour.
// ---------------------------------------------------------------------------

// recursion.c declares `extern volatile int In;` and writes to it
// (`In = fib(10);`) but never defines it — deliberate upstream, so the
// compiler can't dead-code-eliminate the call. Whoever links recursion.o has
// to supply the symbol; wcet_bench/cycle_counter/measure_wcet.cpp does the
// same thing under -DDEFINIR_IN. Must be defined exactly once in the binary.
extern "C" {
volatile int In = 0;
}

extern "C" {
void bench_entry_matmult(void);
void bench_entry_ud(void);
void bench_entry_fft1(void);
int  bench_entry_crc(void);
int  bench_entry_statemate(void);
int  bench_entry_bsort100(void);

// -- int main(...) ----------------------------------------------------------
int bench_entry_adpcm(void);
int bench_entry_cnt(void);
int bench_entry_compress(void);
int bench_entry_cover(void);
int bench_entry_edn(void);
int bench_entry_fac(void);
int bench_entry_fdct(void);
int bench_entry_fibcall(void);
int bench_entry_fir(void);
int bench_entry_insertsort(void);
int bench_entry_janne_complex(void);
int bench_entry_lcdnum(void);
int bench_entry_lms(void);
int bench_entry_ludcmp(void);
int bench_entry_minver(void);
int bench_entry_ndes(void);
int bench_entry_nsichneu(void);
int bench_entry_prime(void);
int bench_entry_qurt(void);

// -- void main(void) ----------------------------------------------------------
void bench_entry_duff(void);
void bench_entry_expint(void);
void bench_entry_jfdctint(void);
void bench_entry_ns(void);
void bench_entry_recursion(void);

// -- implicit-int main() (K&R) ----------------------------------------------
int bench_entry_bs(void);
int bench_entry_whet(void);
}

namespace {

// wcet_bench sources keep their working data in global/static state (e.g.
// matmult.c's file-scope `ArrayA`/`ArrayB`/`ResultArray`, recursion.c's
// `In`) — fine for the single-threaded bare-metal target they were written
// for, but not here: two subtasks that draw the SAME benchmark name can
// land on different cores and execute concurrently, racing on that shared
// state. Reproduced directly: two subtasks both running "whet" on different
// cores crashed `evaluation` with a segfault within seconds. Each benchmark
// gets its own mutex so a second concurrent caller waits instead of racing.
// This does mean a subtask can occasionally block on another core's subtask
// when they happen to share a benchmark and overlap in time — rare, and a
// slow-but-correct run beats a crash — but it is a real inter-core
// dependency the project's timing analysis does not model.
template <typename Fn>
inline void serialized(std::mutex& mtx, Fn&& fn) {
    std::lock_guard<std::mutex> lock(mtx);
    fn();
}

std::mutex mtx_matmult, mtx_ud, mtx_fft1, mtx_crc, mtx_statemate, mtx_bsort100;
void call_matmult()   { serialized(mtx_matmult,   [] { bench_entry_matmult(); }); }
void call_ud()        { serialized(mtx_ud,        [] { bench_entry_ud(); }); }
void call_fft1()      { serialized(mtx_fft1,      [] { bench_entry_fft1(); }); }
void call_crc()       { serialized(mtx_crc,       [] { (void)bench_entry_crc(); }); }
void call_statemate() { serialized(mtx_statemate, [] { (void)bench_entry_statemate(); }); }
void call_bsort100()  { serialized(mtx_bsort100,  [] { (void)bench_entry_bsort100(); }); }

std::mutex mtx_adpcm, mtx_cnt, mtx_compress, mtx_cover, mtx_edn, mtx_fac, mtx_fdct,
           mtx_fibcall, mtx_fir, mtx_insertsort, mtx_janne_complex, mtx_lcdnum,
           mtx_lms, mtx_ludcmp, mtx_minver, mtx_ndes, mtx_nsichneu, mtx_prime, mtx_qurt;
void call_adpcm()         { serialized(mtx_adpcm,         [] { (void)bench_entry_adpcm(); }); }
void call_cnt()           { serialized(mtx_cnt,           [] { (void)bench_entry_cnt(); }); }
void call_compress()      { serialized(mtx_compress,      [] { (void)bench_entry_compress(); }); }
void call_cover()         { serialized(mtx_cover,         [] { (void)bench_entry_cover(); }); }
void call_edn()           { serialized(mtx_edn,           [] { (void)bench_entry_edn(); }); }
void call_fac()           { serialized(mtx_fac,           [] { (void)bench_entry_fac(); }); }
void call_fdct()          { serialized(mtx_fdct,          [] { (void)bench_entry_fdct(); }); }
void call_fibcall()       { serialized(mtx_fibcall,       [] { (void)bench_entry_fibcall(); }); }
void call_fir()           { serialized(mtx_fir,           [] { (void)bench_entry_fir(); }); }
void call_insertsort()    { serialized(mtx_insertsort,    [] { (void)bench_entry_insertsort(); }); }
void call_janne_complex() { serialized(mtx_janne_complex, [] { (void)bench_entry_janne_complex(); }); }
void call_lcdnum()        { serialized(mtx_lcdnum,        [] { (void)bench_entry_lcdnum(); }); }
void call_lms()           { serialized(mtx_lms,           [] { (void)bench_entry_lms(); }); }
void call_ludcmp()        { serialized(mtx_ludcmp,        [] { (void)bench_entry_ludcmp(); }); }
void call_minver()        { serialized(mtx_minver,        [] { (void)bench_entry_minver(); }); }
void call_ndes()          { serialized(mtx_ndes,          [] { (void)bench_entry_ndes(); }); }
void call_nsichneu()      { serialized(mtx_nsichneu,      [] { (void)bench_entry_nsichneu(); }); }
void call_prime()         { serialized(mtx_prime,         [] { (void)bench_entry_prime(); }); }
void call_qurt()          { serialized(mtx_qurt,          [] { (void)bench_entry_qurt(); }); }

std::mutex mtx_duff, mtx_expint, mtx_jfdctint, mtx_ns, mtx_recursion;
void call_duff()      { serialized(mtx_duff,      [] { bench_entry_duff(); }); }
void call_expint()    { serialized(mtx_expint,    [] { bench_entry_expint(); }); }
void call_jfdctint()  { serialized(mtx_jfdctint,  [] { bench_entry_jfdctint(); }); }
void call_ns()        { serialized(mtx_ns,        [] { bench_entry_ns(); }); }
void call_recursion() { serialized(mtx_recursion, [] { bench_entry_recursion(); }); }

std::mutex mtx_bs, mtx_whet;
void call_bs()   { serialized(mtx_bs,   [] { (void)bench_entry_bs(); }); }
void call_whet() { serialized(mtx_whet, [] { (void)bench_entry_whet(); }); }

const std::map<std::string, bench::entry_fn>& table() {
    static const std::map<std::string, bench::entry_fn> t = {
        { "matmult",   &call_matmult   },
        { "bsort100",  &call_bsort100  },
        { "crc",       &call_crc       },
        { "ud",        &call_ud        },
        { "fft1",      &call_fft1      },
        { "statemate", &call_statemate },

        { "adpcm",         &call_adpcm         },
        { "cnt",           &call_cnt           },
        { "compress",      &call_compress      },
        { "cover",         &call_cover         },
        { "edn",           &call_edn           },
        { "fac",           &call_fac           },
        { "fdct",          &call_fdct          },
        { "fibcall",       &call_fibcall       },
        { "fir",           &call_fir           },
        { "insertsort",    &call_insertsort    },
        { "janne_complex", &call_janne_complex },
        { "lcdnum",        &call_lcdnum        },
        { "lms",           &call_lms           },
        { "ludcmp",        &call_ludcmp        },
        { "minver",        &call_minver        },
        { "ndes",          &call_ndes          },
        { "nsichneu",      &call_nsichneu      },
        { "prime",         &call_prime         },
        { "qurt",          &call_qurt          },

        { "duff",      &call_duff      },
        { "expint",    &call_expint    },
        { "jfdctint",  &call_jfdctint  },
        { "ns",        &call_ns        },
        { "recursion", &call_recursion },

        { "bs",   &call_bs   },
        { "whet", &call_whet },
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
