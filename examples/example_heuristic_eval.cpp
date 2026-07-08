/**
 * example_heuristic_eval.cpp
 *
 * Gera N grupos de tarefas (source → intermediate → sink) com períodos e
 * prioridades variados (seguindo a estrutura do deployment_plan.json), aplica
 * cada heurística de bin-packing (FF, BF, WF) para atribuir cores, executa o
 * runtime do MCFlow e compara latência, jitter e deadline misses.
 *
 * Uso:
 *   ./example_heuristic_eval [num_grupos] [num_cores]
 *   sudo ./example_heuristic_eval ...   (SCHED_FIFO — medições mais limpas)
 *
 * Tipos de tasks (ciclam a cada 6 grupos, como no deployment_plan.json):
 *   Tipo 0: período 1ms,  prioridade 80
 *   Tipo 1: período 2ms,  prioridade 65
 *   Tipo 2: período 6ms,  prioridade 50
 *   Tipo 3: período 12ms, prioridade 35
 *   Tipo 4: período 24ms, prioridade 20
 *   Tipo 5: período 48ms, prioridade  6
 *
 * Estratégia de medição:
 *   - Tick loop a cada 1ms (menor período); cada grupo dispara quando
 *     tick % (período/1ms) == 0.
 *   - fire_time[g] guarda o instante em que o grupo g foi notificado no tick
 *     corrente; todos os estágios (source, intermediate, sink) do grupo g
 *     medem latência relativa a esse zero.
 *   - Subtasks rodam em modo aperiódico (period_ns=0 no Subtask) para evitar
 *     drift de next_release_ns com múltiplos subtasks por core.
 *   - Warmup = 3 ciclos do LCM (144ms); Medição = 10 ciclos (480ms).
 *   - Jitter = lat_max − lat_min (peak-to-peak).
 */

#include <iostream>
#include <iomanip>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <time.h>

#include "deployment_plan.hpp"
#include "allocator.hpp"
#include "team_manager.hpp"
#include "dag.hpp"
#include "dispatcher.hpp"

// ---------------------------------------------------------------------------
//  WCET por tipo de componente (Heptane MIPS 100 MHz — docs/wcet_heptane.md)
// ---------------------------------------------------------------------------
static constexpr uint64_t WCET_SOURCE       = 240840;
static constexpr uint64_t WCET_INTERMEDIATE = 354240;
static constexpr uint64_t WCET_SINK         = 352160;

// ---------------------------------------------------------------------------
//  Tipos de task (ciclo de 6, igual ao deployment_plan.json)
// ---------------------------------------------------------------------------
struct TaskType {
    uint64_t period_ns;
    int      priority;
};

static constexpr TaskType TASK_TYPES[] = {
    {  1'000'000, 80 },
    {  2'000'000, 65 },
    {  6'000'000, 50 },
    { 12'000'000, 35 },
    { 24'000'000, 20 },
    { 48'000'000,  6 },
};
static constexpr int      NUM_TASK_TYPES = 6;
static constexpr uint64_t MIN_PERIOD_NS  = 1'000'000;   // 1ms — menor período
static constexpr uint64_t LCM_NS         = 48'000'000;  // LCM(1,2,6,12,24,48)ms
static constexpr int      LCM_TICKS      = static_cast<int>(LCM_NS / MIN_PERIOD_NS); // 48
static constexpr int      WARMUP_TICKS   = 3  * LCM_TICKS;  // 144 ticks = 144 ms
static constexpr int      MEASURE_TICKS  = 10 * LCM_TICKS;  // 480 ticks = 480 ms

// ---------------------------------------------------------------------------
//  Resultado agregado de uma rodada de avaliação
// ---------------------------------------------------------------------------
struct EvalResult {
    std::string heuristic;
    int    num_cores;
    double mean_latency_us;
    double min_latency_us;
    double max_latency_us;
    double jitter_us;       // max - min (peak-to-peak)
    int    total_misses;
    int    total_jobs;
    std::vector<double> core_util;
    bool   feasible;
};

// ---------------------------------------------------------------------------
//  Gera o plano de subtarefas (sem core atribuído) para N grupos
// ---------------------------------------------------------------------------
static std::vector<SubtaskInfo> make_subtasks(int num_groups) {
    std::vector<SubtaskInfo> subtasks;
    int id = 1;
    for (int g = 0; g < num_groups; ++g) {
        const TaskType& tt = TASK_TYPES[g % NUM_TASK_TYPES];

        auto make = [&](const std::string& type, uint64_t wcet) {
            SubtaskInfo s;
            s.id             = id++;
            s.task_id        = g + 1;
            s.component_type = type;
            s.core           = -1;
            s.priority       = tt.priority;
            s.period_ns      = tt.period_ns;
            s.deadline_ns    = tt.period_ns;
            s.wcet_ns        = wcet;
            return s;
        };
        subtasks.push_back(make("source",       WCET_SOURCE));
        subtasks.push_back(make("intermediate", WCET_INTERMEDIATE));
        subtasks.push_back(make("sink",         WCET_SINK));
    }
    return subtasks;
}

// ---------------------------------------------------------------------------
//  Gera as conexões: source → intermediate → sink dentro de cada grupo
// ---------------------------------------------------------------------------
static std::vector<ConnectionInfo> make_connections(int num_groups) {
    std::vector<ConnectionInfo> conns;
    for (int g = 0; g < num_groups; ++g) {
        int base = g * 3 + 1;
        conns.push_back({ base,     base + 1 });
        conns.push_back({ base + 1, base + 2 });
    }
    return conns;
}

// ---------------------------------------------------------------------------
//  Executa uma rodada completa e retorna métricas agregadas
// ---------------------------------------------------------------------------
static EvalResult run_eval(const std::string& heuristic_name,
                           const AllocationResult& alloc,
                           const std::vector<ConnectionInfo>& conns,
                           int num_cores) {
    const auto& subtasks = alloc.subtasks;
    int max_id = 0;
    for (const auto& s : subtasks) max_id = std::max(max_id, s.id);

    int num_groups = static_cast<int>(subtasks.size()) / 3;

    // fire_time[g]: instante em que o grupo g foi disparado no tick corrente.
    // Todos os estágios do grupo medem latência relativa a esse zero.
    auto fire_buf = std::make_unique<std::atomic<uint64_t>[]>(num_groups);
    for (int i = 0; i < num_groups; ++i) fire_buf[i].store(0, std::memory_order_relaxed);
    auto* fire_time = fire_buf.get();

    // Ativado após os ticks de warmup; lambdas ignoram amostras antes disso.
    std::atomic<bool> measuring{false};

    // Slots de dados compartilhados (simulação do pipeline)
    auto vals_buf = std::make_unique<std::atomic<double>[]>(max_id + 1);
    for (int i = 0; i <= max_id; ++i) vals_buf[i].store(0.0, std::memory_order_relaxed);
    auto* v = vals_buf.get();

    // Mapa predecessor: downstream_id → [upstream_ids]
    std::map<int, std::vector<int>> preds;
    for (const auto& c : conns) preds[c.downstream].push_back(c.upstream);

    // Métricas por subtask
    struct Metrics {
        std::vector<int64_t> latency_ns;
        int64_t min_lat = std::numeric_limits<int64_t>::max();
        int64_t max_lat = std::numeric_limits<int64_t>::min();
        int64_t sum_lat = 0;
        int     misses  = 0;
    };
    std::map<int, Metrics> mmap;

    int ticks = WARMUP_TICKS + MEASURE_TICKS;

    for (const auto& s : subtasks) {
        // reserva máximo: 1ms group dispara MEASURE_TICKS vezes
        mmap[s.id].latency_ns.reserve(MEASURE_TICKS + 4);
    }

    // Objetos Subtask
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;
    for (const auto& s : subtasks)
        subtask_ptrs[s.id] = std::make_unique<Subtask>(s.id, []{});

    // Lambdas de execução — latência medida relativa a fire_time[gid]
    for (const auto& info : subtasks) {
        int      id  = info.id;
        int      gid = info.task_id - 1;  // índice do grupo (0-based)
        Subtask* s   = subtask_ptrs.at(id).get();
        auto&    m   = mmap.at(id);
        uint64_t dl  = info.deadline_ns;

        auto record = [&m, fire_time, gid, dl, &measuring](uint64_t t) {
            if (!measuring.load(std::memory_order_relaxed)) return;
            int64_t lat = static_cast<int64_t>(t)
                        - static_cast<int64_t>(fire_time[gid].load(std::memory_order_relaxed));
            if (lat < 0) return; // amostra de ciclo anterior — descarta
            m.latency_ns.push_back(lat);
            m.sum_lat += lat;
            if (lat < m.min_lat) m.min_lat = lat;
            if (lat > m.max_lat) m.max_lat = lat;
            if (static_cast<uint64_t>(lat) > dl) ++m.misses;
        };

        if (info.component_type == "source") {
            s->execute = [v, id, record] {
                uint64_t t = Dispatcher::monotonic_ns();
                v[id].store(v[id].load(std::memory_order_relaxed) + 1.0,
                            std::memory_order_relaxed);
                record(t);
            };
        } else if (info.component_type == "intermediate") {
            int pred = preds.at(id)[0];
            s->execute = [v, id, pred, record] {
                uint64_t t = Dispatcher::monotonic_ns();
                v[id].store(v[pred].load(std::memory_order_relaxed) * 2.0,
                            std::memory_order_relaxed);
                record(t);
            };
        } else { // sink
            int pred = preds.at(id)[0];
            s->execute = [v, pred, record] {
                uint64_t t = Dispatcher::monotonic_ns();
                (void)v[pred].load(std::memory_order_relaxed);
                record(t);
            };
        }
    }

    // DAG
    DAG dag;
    for (const auto& s : subtasks) dag.add_node(s.id, nullptr);
    for (const auto& c : conns)    dag.add_edge(c.upstream, c.downstream);

    // Entradas do TeamManager (usa period_ns original para sizing do ring buffer)
    std::vector<TeamManager::SubtaskEntry> entries;
    for (const auto& s : subtasks)
        entries.push_back({ s, subtask_ptrs.at(s.id).get() });

    TeamManager tm;
    tm.initialize(entries, dag);

    // Desabilita o release-guard periódico: subtasks disparam na chegada de
    // notify(), sem drift de next_release_ns entre subtasks no mesmo core.
    for (const auto& s : subtasks)
        subtask_ptrs.at(s.id)->period_ns = 0;

    tm.start();

    // Tick loop — 1 tick por ms; cada grupo dispara quando tick % ratio == 0
    uint64_t next_tick = Dispatcher::monotonic_ns() + MIN_PERIOD_NS;
    for (int tick = 1; tick <= ticks; ++tick) {
        struct timespec ts;
        ts.tv_sec  = next_tick / 1'000'000'000ULL;
        ts.tv_nsec = next_tick % 1'000'000'000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

        if (tick > WARMUP_TICKS)
            measuring.store(true, std::memory_order_relaxed);

        for (int g = 0; g < num_groups; ++g) {
            uint64_t period_g = subtasks[static_cast<size_t>(g * 3)].period_ns;
            uint64_t ratio    = period_g / MIN_PERIOD_NS;
            if (static_cast<uint64_t>(tick) % ratio == 0) {
                fire_time[g].store(Dispatcher::monotonic_ns(), std::memory_order_relaxed);
                tm.notify(g * 3 + 1);
            }
        }

        next_tick += MIN_PERIOD_NS;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    // Agregação
    int64_t all_min = std::numeric_limits<int64_t>::max();
    int64_t all_max = std::numeric_limits<int64_t>::min();
    int64_t all_sum = 0;
    int     all_jobs = 0, all_misses = 0;

    for (auto& [id, m] : mmap) {
        if (m.latency_ns.empty()) continue;
        all_min    = std::min(all_min,  m.min_lat);
        all_max    = std::max(all_max,  m.max_lat);
        all_sum   += m.sum_lat;
        all_jobs  += static_cast<int>(m.latency_ns.size());
        all_misses += m.misses;
    }

    EvalResult r;
    r.heuristic       = heuristic_name;
    r.num_cores       = num_cores;
    r.mean_latency_us = all_jobs > 0 ? (all_sum / 1000.0) / all_jobs : 0.0;
    r.min_latency_us  = all_jobs > 0 ? all_min / 1000.0 : 0.0;
    r.max_latency_us  = all_jobs > 0 ? all_max / 1000.0 : 0.0;
    r.jitter_us       = all_jobs > 0 ? (all_max - all_min) / 1000.0 : 0.0;
    r.total_misses    = all_misses;
    r.total_jobs      = all_jobs;
    r.core_util       = alloc.core_util;
    r.feasible        = alloc.feasible;
    return r;
}

// ---------------------------------------------------------------------------
//  Impressão da tabela de resultados
// ---------------------------------------------------------------------------
static void print_table(const std::string& title,
                        const std::vector<EvalResult>& results,
                        int num_cores) {
    const int W = 100;
    std::cout << "\n" << title << "\n";
    std::cout << std::string(W, '=') << "\n";
    std::cout << std::left
              << std::setw(12) << "Heuristic"
              << std::setw(14) << "Min(us)"
              << std::setw(14) << "Mean(us)"
              << std::setw(14) << "Max(us)"
              << std::setw(14) << "Jitter(us)"
              << std::setw(9)  << "Misses"
              << std::setw(7)  << "Jobs"
              << std::setw(8)  << "Miss%"
              << "Util/core\n";
    std::cout << std::string(W, '-') << "\n";

    for (const auto& r : results) {
        if (!r.feasible) {
            std::cout << std::setw(12) << r.heuristic << "INFEASIBLE\n";
            continue;
        }
        double miss_pct = r.total_jobs > 0 ? 100.0 * r.total_misses / r.total_jobs : 0.0;

        std::cout << std::fixed
                  << std::setw(12) << r.heuristic
                  << std::setw(14) << std::setprecision(1) << r.min_latency_us
                  << std::setw(14) << std::setprecision(1) << r.mean_latency_us
                  << std::setw(14) << std::setprecision(1) << r.max_latency_us
                  << std::setw(14) << std::setprecision(1) << r.jitter_us
                  << std::setw(9)  << r.total_misses
                  << std::setw(7)  << r.total_jobs
                  << std::setprecision(2) << std::setw(7) << miss_pct << "%  ";

        for (int c = 0; c < num_cores; ++c)
            std::cout << "C" << c << "=" << std::setprecision(3) << r.core_util[c] << " ";
        std::cout << "\n";
    }
    std::cout << std::string(W, '=') << "\n";
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int num_groups = (argc > 1) ? std::stoi(argv[1]) : 6;
    int num_cores  = (argc > 2) ? std::stoi(argv[2]) : 3;

    auto subtasks = make_subtasks(num_groups);
    auto conns    = make_connections(num_groups);

    double total_util = 0.0;
    for (const auto& s : subtasks) total_util += allocator::utilization(s);

    std::cout << "=== Bin-Packing Heuristic Evaluation ===\n"
              << "Groups: "      << num_groups
              << " | Subtasks: " << subtasks.size()
              << " | Cores: "    << num_cores << "\n"
              << "Task types: " << NUM_TASK_TYPES << " (";
    for (int i = 0; i < NUM_TASK_TYPES; ++i)
        std::cout << TASK_TYPES[i].period_ns / 1'000'000 << "ms" << (i < NUM_TASK_TYPES-1 ? "," : "");
    std::cout << ") | Warmup: " << WARMUP_TICKS << "ms | Measurement: " << MEASURE_TICKS << "ms\n"
              << std::fixed << std::setprecision(4)
              << "Total util: " << total_util << " / " << num_cores << ".0000\n";

    struct Run { std::string name; AllocationResult alloc; };
    std::vector<Run> runs = {
        { "First Fit", allocator::first_fit(subtasks,  num_cores) },
        { "Best Fit",  allocator::best_fit(subtasks,   num_cores) },
        { "Worst Fit", allocator::worst_fit(subtasks,  num_cores) },
    };

    bool any_infeasible = false;
    for (auto& run : runs) {
        if (!run.alloc.feasible) {
            std::cout << "[" << run.name << "] INFEASIBLE — could not place all subtasks, skipping.\n";
            any_infeasible = true;
        }
    }
    if (any_infeasible) std::cout << "\n";

    std::vector<EvalResult> results;
    for (auto& run : runs) {
        if (!run.alloc.feasible) {
            EvalResult r{};
            r.heuristic = run.name;
            r.feasible  = false;
            results.push_back(r);
            continue;
        }
        std::cout << "Running " << run.name << "...\n";
        results.push_back(run_eval(run.name, run.alloc, conns, num_cores));
    }

    print_table("Latency — all subtasks", results, num_cores);

    return 0;
}
