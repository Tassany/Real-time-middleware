/**
 * example_wcet_measure.cpp
 *
 * Mede empiricamente o tempo de execução de cada componente WCET
 * (wcet_components/{source,intermediate,sink}.c, expostos via
 * wcet_components_rt.hpp) rodando muitas vezes no hardware alvo — uma
 * alternativa ao WCET estático do Heptane (docs/wcet_heptane.md): não é um
 * bound formal de pior caso, é uma medida empírica de tempo real, em
 * nanossegundos, sem nenhuma suposição de frequência de clock ou modelo de
 * CPU. Roda no processador que executar o binário (dev machine, Raspberry Pi
 * etc.), então o número já reflete o hardware de verdade.
 *
 * Uso:
 *   ./example_wcet_measure [iters]      (default: 100000)
 *   taskset -c 0 ./example_wcet_measure ...   (fixa o core manualmente, opcional)
 */

#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <time.h>
#include <sched.h>
#include <pthread.h>

#include "wcet_components/wcet_components_rt.hpp"

// ---------------------------------------------------------------------------
//  Referência: ciclos ARM medidos pelo Heptane (docs/wcet_heptane.md) — só
//  para comparação na tabela final, não entra em nenhum cálculo aqui.
// ---------------------------------------------------------------------------
static constexpr uint64_t HEPTANE_ARM_CYCLES_SOURCE       = 3761;
static constexpr uint64_t HEPTANE_ARM_CYCLES_INTERMEDIATE = 74474;
static constexpr uint64_t HEPTANE_ARM_CYCLES_SINK         = 29931;

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

struct Stats {
    double   min_ns  = 0.0;
    double   mean_ns = 0.0;
    double   max_ns  = 0.0;
    uint64_t samples = 0;
};

template <class Fn>
static Stats measure(Fn&& fn, int warmup, int iters) {
    for (int i = 0; i < warmup; ++i) fn();

    std::vector<int64_t> lat;
    lat.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = now_ns();
        fn();
        uint64_t t1 = now_ns();
        lat.push_back(static_cast<int64_t>(t1 - t0));
    }

    Stats s;
    s.samples = lat.size();
    s.min_ns  = static_cast<double>(*std::min_element(lat.begin(), lat.end()));
    s.max_ns  = static_cast<double>(*std::max_element(lat.begin(), lat.end()));
    double sum = 0.0;
    for (auto v : lat) sum += static_cast<double>(v);
    s.mean_ns = sum / static_cast<double>(lat.size());
    return s;
}

static void print_row(const std::string& name, const Stats& s, uint64_t heptane_cycles) {
    std::cout << std::left  << std::setw(14) << name
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(12) << s.min_ns
              << std::setw(14) << s.mean_ns
              << std::setw(14) << s.max_ns
              << std::setw(12) << (s.mean_ns / 1000.0)
              << std::setw(12) << s.samples
              << std::setw(16) << heptane_cycles
              << "\n";
}

int main(int argc, char** argv) {
    int iters  = (argc > 1) ? std::atoi(argv[1]) : 100000;
    int warmup = std::max(1000, iters / 100);

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0)
        std::cerr << "[warn] não foi possível fixar afinidade no core 0 "
                     "(rode com taskset/sudo para medições mais limpas)\n";

    // Acumuladores voláteis: impedem o compilador de eliminar as chamadas
    // por dead-code elimination (o resultado nunca é "usado" de outra forma).
    volatile int   sink_int   = 0;
    volatile float sink_float = 0.0f;

    // source: binary_search — mesma chave usada no main() original de bs.c
    Stats st_source = measure(
        [&] { sink_int ^= wcet_rt::binary_search_run(8); }, warmup, iters);

    // intermediate: jpeg_fdct_islow — buffer próprio, seed igual ao main()
    // original de jfdctint.c. A função é in-place e o número de
    // instruções não depende dos dados, então não precisa resetar entre
    // chamadas.
    int dct_buf[64];
    {
        int seed = 1;
        for (int i = 0; i < 64; ++i) {
            seed = (seed * 133 + 81) % 65535;
            dct_buf[i] = seed;
        }
    }
    Stats st_inter = measure(
        [&] { wcet_rt::dct_run(dct_buf); sink_int ^= dct_buf[0]; }, warmup, iters);

    // sink: my_sqrt — mesmo valor do main() original de sqrt.c (evita o
    // atalho val==0 que pula o laço inteiro)
    Stats st_sink = measure(
        [&] { sink_float += wcet_rt::sqrt_run(19.5f); }, warmup, iters);

    std::cout << "=== WCET empírico (tempo médio real, sem Heptane) ===\n"
              << "Iterações medidas: " << iters << "  (warm-up: " << warmup << ")\n"
              << "sink_int=" << sink_int << " sink_float=" << sink_float
              << "  (ignorar — só evita eliminação de código morto)\n\n";

    std::cout << std::left  << std::setw(14) << "Componente"
              << std::right << std::setw(12) << "Min(ns)"
              << std::setw(14) << "Mean(ns)"
              << std::setw(14) << "Max(ns)"
              << std::setw(12) << "Mean(us)"
              << std::setw(12) << "Amostras"
              << std::setw(16) << "Heptane(ciclos)"
              << "\n" << std::string(94, '-') << "\n";

    print_row("source",       st_source, HEPTANE_ARM_CYCLES_SOURCE);
    print_row("intermediate", st_inter,  HEPTANE_ARM_CYCLES_INTERMEDIATE);
    print_row("sink",         st_sink,   HEPTANE_ARM_CYCLES_SINK);
    std::cout << std::string(94, '-') << "\n\n";

    std::ofstream csv("wcet_measured.csv");
    csv << "component,min_ns,mean_ns,max_ns,samples\n";
    csv << std::fixed << std::setprecision(1);
    csv << "source,"       << st_source.min_ns << ',' << st_source.mean_ns << ',' << st_source.max_ns << ',' << st_source.samples << '\n';
    csv << "intermediate," << st_inter.min_ns  << ',' << st_inter.mean_ns  << ',' << st_inter.max_ns  << ',' << st_inter.samples  << '\n';
    csv << "sink,"         << st_sink.min_ns   << ',' << st_sink.mean_ns   << ',' << st_sink.max_ns   << ',' << st_sink.samples   << '\n';
    std::cout << "Resultados escritos em wcet_measured.csv\n";

    return 0;
}
