/**
 * example_overhead_pilot.cpp
 *
 * Fase 2 do roteiro DRU (ver plano em .claude/plans/): mede o overhead do
 * próprio middleware, isolado de qualquer benchmark real, para decidir se
 * ele é "ruído" (desprezível) ou "sinal" (dominante) frente aos WCETs
 * medidos em wcet_bench/cycle_counter/RELATORIO.md antes de autorizar a
 * campanha WF+DRU (Fase 3). Se o overhead for comparável ao menor WCET
 * medido (statemate, ~207ns no Pi 5), a campanha mediria o middleware, não
 * a política de alocação.
 *
 * Três cenários, cada um com sua própria contagem de amostras (warmup
 * descartado, ver 'warmup' abaixo):
 *
 *   1. dispatch_vazio : uma subtask solo (sem downstream), execute() vazio.
 *      Isola o custo do mecanismo de despacho em si — fila, guarda de
 *      6 passos (dispatcher.hpp), timer — sem nenhuma computação real nem
 *      propagação a jusante.
 *   2. aresta_intra   : cadeia de 2 nós no MESMO core/Dispatcher. O notify()
 *      do produtor empurra o consumidor na mesma fila que o laço de dreno
 *      (loop() em dispatcher.hpp) já está varrendo na própria thread —
 *      sem troca de contexto nem syscall de acordar outra thread.
 *   3. aresta_inter   : cadeia de 2 nós em CORES DIFERENTES. O notify()
 *      escreve num eventfd de uma thread diferente, que só é atendido
 *      quando o epoll_wait daquela thread acorda — é o custo real de
 *      cruzar de um core para outro.
 *
 * Disparo: mesmo padrão de scripts/evaluation.cpp — clock_nanosleep com
 * TIMER_ABSTIME dispara notify() no instante exato do período, e o
 * release-guard do Dispatcher garante o resto; latência = t_atual -
 * t_agendado, com t_agendado = next_release_ns - period_ns (o
 * next_release_ns já foi avançado pelo passo 4b antes de execute() rodar).
 *
 * Condições de bancada (sem isolcpus, decisão do usuário para este piloto):
 * só afinidade de core (embutida no Dispatcher) + SCHED_FIFO via sudo. Sem
 * isolar os cores no kernel, outras threads/IRQs do sistema podem competir
 * pelos mesmos cores — refazer sob isolamento completo antes de usar este
 * número como métrica definitiva, caso ele fique perto do limiar de decisão.
 *
 * O resultado entra como nota qualitativa de threat-to-validity no
 * relatório, não como correção quantitativa na Fase 3 — este programa só
 * mede e imprime, a comparação com a tabela de WCET é manual (ver
 * scripts/random_plan.py WCET_NS ou wcet_bench/cycle_counter/RELATORIO.md).
 *
 * Uso:
 *   sudo ./example_overhead_pilot [amostras] [core_a] [core_b]
 */

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <time.h>

#include "dispatcher.hpp"

namespace {

constexpr uint64_t PERIOD_NS = 1'000'000ULL; // 1ms — menor período usado na grade dos planos (random_plan.py)
constexpr int      WARMUP    = 20;           // descarta a inicialização de next_release_ns e o aquecimento de cache

struct Stats {
    double mean_ns, median_ns, p99_ns, max_ns, jitter_ns;
};

Stats summarize(std::vector<double> samples_ns) {
    std::sort(samples_ns.begin(), samples_ns.end());
    const std::size_t n = samples_ns.size();
    const double sum = std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0);
    const std::size_t p99_idx = static_cast<std::size_t>(0.99 * (n - 1));
    return Stats{ sum / n, samples_ns[n / 2], samples_ns[p99_idx],
                  samples_ns.back(), samples_ns.back() - samples_ns.front() };
}

void print_row(const std::string& name, const Stats& s) {
    std::cout << std::left << std::setw(16) << name << std::right
              << std::fixed << std::setprecision(1)
              << std::setw(12) << s.mean_ns
              << std::setw(12) << s.median_ns
              << std::setw(12) << s.p99_ns
              << std::setw(12) << s.max_ns
              << std::setw(12) << s.jitter_ns << "\n";
}

void write_csv(const std::string& path, const std::vector<double>& samples_ns) {
    std::ofstream f(path);
    f << "sample,latency_ns\n";
    for (std::size_t i = 0; i < samples_ns.size(); ++i)
        f << i << "," << static_cast<uint64_t>(samples_ns[i]) << "\n";
}

// Dispara `total` releases de `solo` espaçados exatamente por PERIOD_NS,
// com sleep de tempo absoluto (mesmo padrão de scripts/evaluation.cpp) para
// que atrasos de um tick não se acumulem nos seguintes.
void drive(Dispatcher& disp, Subtask& solo, int total) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t next_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                        static_cast<uint64_t>(ts.tv_nsec);

    for (int i = 0; i < total; ++i) {
        next_ns += PERIOD_NS;
        ts.tv_sec  = static_cast<time_t>(next_ns / 1'000'000'000ULL);
        ts.tv_nsec = static_cast<long>(next_ns % 1'000'000'000ULL);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        disp.notify(&solo);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const int samples = (argc > 1) ? std::stoi(argv[1]) : 2000;
    const int core_a  = (argc > 2) ? std::stoi(argv[2]) : 0;
    const int core_b  = (argc > 3) ? std::stoi(argv[3]) : 1;
    const int total   = samples + WARMUP;

    std::cout << "=== Piloto de overhead (Fase 2) ===\n"
              << "amostras/cenario: " << samples << " (+" << WARMUP << " warmup)"
              << "  cores: " << core_a << "," << core_b << "\n\n";

    // --- Cenario 1: dispatch_vazio ------------------------------------------
    std::vector<double> lat_vazio(samples);
    {
        Dispatcher disp(core_a, 50);
        Subtask solo;
        solo.id        = 1;
        solo.period_ns = PERIOD_NS;

        int seen = 0;
        solo.execute = [&]() {
            const uint64_t t_actual = Dispatcher::monotonic_ns();
            const uint64_t t_sched  = solo.next_release_ns - solo.period_ns;
            ++seen;
            if (seen > WARMUP)
                lat_vazio[seen - WARMUP - 1] = static_cast<double>(t_actual - t_sched);
        };

        disp.start();
        drive(disp, solo, total);
        disp.stop();
    }

    // --- Cenario 2: aresta_intra (mesmo core) -------------------------------
    std::vector<double> lat_intra(samples);
    {
        Dispatcher disp(core_a, 50);
        Subtask producer, consumer;
        producer.id        = 1;
        producer.period_ns = PERIOD_NS;
        consumer.id        = 2;

        std::atomic<uint64_t> produce_ts_ns{0};
        int seen = 0;

        producer.execute = [&]() {
            produce_ts_ns.store(Dispatcher::monotonic_ns(), std::memory_order_release);
        };
        consumer.execute = [&]() {
            const uint64_t t_consume = Dispatcher::monotonic_ns();
            const uint64_t t_produce = produce_ts_ns.load(std::memory_order_acquire);
            ++seen;
            if (seen > WARMUP)
                lat_intra[seen - WARMUP - 1] = static_cast<double>(t_consume - t_produce);
        };

        producer.downstream.push_back({&disp, &consumer});
        disp.register_subtask(&producer);
        disp.register_subtask(&consumer);

        disp.start();
        drive(disp, producer, total);
        disp.stop();
    }

    // --- Cenario 3: aresta_inter (cores diferentes) -------------------------
    std::vector<double> lat_inter(samples);
    {
        Dispatcher disp_a(core_a, 50);
        Dispatcher disp_b(core_b, 50);
        Subtask producer, consumer;
        producer.id        = 1;
        producer.period_ns = PERIOD_NS;
        consumer.id        = 2;

        std::atomic<uint64_t> produce_ts_ns{0};
        int seen = 0;

        producer.execute = [&]() {
            produce_ts_ns.store(Dispatcher::monotonic_ns(), std::memory_order_release);
        };
        consumer.execute = [&]() {
            const uint64_t t_consume = Dispatcher::monotonic_ns();
            const uint64_t t_produce = produce_ts_ns.load(std::memory_order_acquire);
            ++seen;
            if (seen > WARMUP)
                lat_inter[seen - WARMUP - 1] = static_cast<double>(t_consume - t_produce);
        };

        producer.downstream.push_back({&disp_b, &consumer});
        disp_a.register_subtask(&producer);
        disp_b.register_subtask(&consumer);

        disp_a.start();
        disp_b.start();
        drive(disp_a, producer, total);
        disp_a.stop();
        disp_b.stop();
    }

    std::cout << std::left << std::setw(16) << "cenario" << std::right
              << std::setw(12) << "media_ns" << std::setw(12) << "mediana_ns"
              << std::setw(12) << "p99_ns" << std::setw(12) << "max_ns"
              << std::setw(12) << "jitter_ns" << "\n";
    print_row("dispatch_vazio", summarize(lat_vazio));
    print_row("aresta_intra",   summarize(lat_intra));
    print_row("aresta_inter",   summarize(lat_inter));

    write_csv("overhead_pilot_dispatch_vazio.csv", lat_vazio);
    write_csv("overhead_pilot_aresta_intra.csv",   lat_intra);
    write_csv("overhead_pilot_aresta_inter.csv",   lat_inter);

    std::cout << "\nAmostras brutas: overhead_pilot_{dispatch_vazio,aresta_intra,aresta_inter}.csv\n"
              << "Compare com o menor wcet_ns medido (scripts/random_plan.py WCET_NS ou\n"
              << "wcet_bench/cycle_counter/RELATORIO.md sec. 4) para decidir se este overhead\n"
              << "é ruido ou sinal frente aos benchmarks reais.\n";

    return 0;
}
