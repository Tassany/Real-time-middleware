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
 * Estratégia de medição (idêntica a examples/example_eval.cpp, para que os
 * números sejam diretamente comparáveis no artigo):
 *   - Cada subtask mantém seu period_ns real; o release-guard de 6 passos do
 *     Dispatcher (dispatcher.hpp) fica ativo para todos os estágios.
 *   - Latência de cada job é medida dentro do próprio execute(), usando o
 *     estado do release-guard: t_sched = next_release_ns - period_ns,
 *     lat = t_actual - t_sched. Não há "fire_time" externo por grupo.
 *   - Janela de medição dinâmica: min_p = menor período entre as fontes
 *     (subtasks sem predecessor); lcm_p = LCM de todos os períodos de fonte;
 *     ticks = 4 × lcm_p / min_p. Sem descarte de warmup — mede desde o tick 1.
 *   - Jitter = lat_max − lat_min (peak-to-peak).
 *   - Amostras brutas de cada heurística são exportadas para
 *     latency_heuristic_<nome>.csv, no mesmo formato de latency_samples.csv.
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
#include <fstream>
#include <numeric>
#include <cctype>
#include <array>
#include <cmath>

#include "deployment_plan.hpp"
#include "allocator.hpp"
#include "team_manager.hpp"
#include "dag.hpp"
#include "dispatcher.hpp"
#include "wcet_components/wcet_components_rt.hpp"

// ---------------------------------------------------------------------------
//  WCET por tipo de componente (Heptane, ARM, ciclos — sem conversão de
//  frequência: valores usados como estão, direto em allocator::utilization())
//  source=bs.c/binary_search, intermediate=jfdctint.c/jpeg_fdct_islow,
//  sink=sqrt.c/my_sqrt
// ---------------------------------------------------------------------------
static constexpr uint64_t WCET_SOURCE       = 3761;
static constexpr uint64_t WCET_INTERMEDIATE = 74474;
static constexpr uint64_t WCET_SINK         = 29931;

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
            s.core           = 4; // placeholder, heurística vai atribuir
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
//  Slugifica o nome da heurística para usar como nome de arquivo CSV
// ---------------------------------------------------------------------------
static std::string slugify(const std::string& name) {
    std::string out = name;
    for (auto& c : out)
        c = (c == ' ') ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
//  Executa uma rodada completa e retorna métricas agregadas
//
//  Medição idêntica a examples/example_eval.cpp: period_ns de cada subtask
//  fica intacto (release-guard real do Dispatcher ativo), e a latência de
//  cada job é computada inline em execute() como
//  t_actual - (next_release_ns - period_ns) — sem fire_time externo por
//  grupo e sem descarte de warmup.
// ---------------------------------------------------------------------------
static EvalResult run_eval(const std::string& heuristic_name,
                           const AllocationResult& alloc,
                           const std::vector<ConnectionInfo>& conns,
                           int num_cores) {
    const auto& subtasks = alloc.subtasks;
    int max_id = 0;
    for (const auto& s : subtasks) max_id = std::max(max_id, s.id);

    // Slots de dados compartilhados (simulação do pipeline)
    auto vals_buf = std::make_unique<std::atomic<double>[]>(max_id + 1);
    for (int i = 0; i <= max_id; ++i) vals_buf[i].store(0.0, std::memory_order_relaxed);
    auto* v = vals_buf.get();

    // Mapa predecessor: downstream_id → [upstream_ids]
    std::map<int, std::vector<int>> preds;
    for (const auto& c : conns) preds[c.downstream].push_back(c.upstream);

    // Fontes: subtasks sem predecessor (mesmo critério de example_eval.cpp)
    std::vector<std::pair<int, uint64_t>> sources;
    for (const auto& s : subtasks)
        if (preds.find(s.id) == preds.end())
            sources.push_back({ s.id, s.period_ns });

    uint64_t min_p = sources[0].second;
    for (auto& [id, p] : sources) min_p = std::min(min_p, p);

    uint64_t lcm_p = sources[0].second;
    for (std::size_t i = 1; i < sources.size(); ++i)
        lcm_p = std::lcm(lcm_p, sources[i].second);

    int ticks = static_cast<int>(4 * lcm_p / min_p);

    // Métricas por subtask
    struct Metrics {
        uint64_t period_ns = 0;
        int      core      = 4; // placeholder, heurística vai atribuir
        int      priority  = 0;
        std::vector<int64_t> latency_ns;
        int64_t min_lat = std::numeric_limits<int64_t>::max();
        int64_t max_lat = std::numeric_limits<int64_t>::min();
        int64_t sum_lat = 0;
        int     misses  = 0;
    };
    std::map<int, Metrics> mmap;

    for (const auto& s : subtasks) {
        auto& m    = mmap[s.id];
        m.period_ns = s.period_ns;
        m.core      = s.core;
        m.priority  = s.priority;

        int cap = (s.period_ns > 0)
            ? static_cast<int>(4 * lcm_p / s.period_ns) + 4
            : ticks + 4;
        m.latency_ns.reserve(static_cast<std::size_t>(cap));
    }

    // Objetos Subtask
    std::map<int, std::unique_ptr<Subtask>> subtask_ptrs;
    for (const auto& s : subtasks)
        subtask_ptrs[s.id] = std::make_unique<Subtask>(s.id, []{});

    // Buffer próprio por instância "intermediate" (jpeg_fdct_islow opera
    // in-place; cada subtask precisa do seu próprio array, senão instâncias
    // concorrentes em cores diferentes corrompem o mesmo estado).
    std::map<int, std::unique_ptr<std::array<int, 64>>> dct_state;
    for (const auto& s : subtasks) {
        if (s.component_type != "intermediate") continue;
        auto buf = std::make_unique<std::array<int, 64>>();
        int seed = 1;
        for (int i = 0; i < 64; ++i) {
            seed = (seed * 133 + 81) % 65535;
            (*buf)[static_cast<std::size_t>(i)] = seed;
        }
        dct_state[s.id] = std::move(buf);
    }

    // Lambdas de execução — latência medida via release-guard real do próprio
    // subtask (igual a example_eval.cpp), não via fire_time de grupo. O corpo
    // de cada estágio agora roda o código real analisado pelo Heptane
    // (wcet_components/{source,intermediate,sink}.c, ver
    // wcet_components_rt.hpp), não mais um cálculo trivial.
    for (const auto& info : subtasks) {
        int      id = info.id;
        Subtask*  s = subtask_ptrs.at(id).get();
        auto&     m = mmap.at(id);
        uint64_t dl = info.deadline_ns;

        if (info.component_type == "source") {
            s->execute = [v, id, s, &m, dl] {
                uint64_t t_actual = Dispatcher::monotonic_ns();

                int r = wcet_rt::binary_search_run(8); // mesma chave do main() de bs.c
                v[id].store(static_cast<double>(r), std::memory_order_relaxed);

                if (s->period_ns > 0) {
                    int64_t t_sched = static_cast<int64_t>(
                        s->next_release_ns - s->period_ns);
                    int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                    m.latency_ns.push_back(lat);
                    m.sum_lat += lat;
                    if (lat < m.min_lat) m.min_lat = lat;
                    if (lat > m.max_lat) m.max_lat = lat;
                    if (static_cast<uint64_t>(lat) > dl) ++m.misses;
                }
            };

        } else if (info.component_type == "intermediate") {
            int pred = preds.at(id)[0];
            std::array<int, 64>* dct = dct_state.at(id).get();
            s->execute = [v, id, pred, s, &m, dl, dct] {
                uint64_t t_actual = Dispatcher::monotonic_ns();

                (void)v[pred].load(std::memory_order_relaxed);
                wcet_rt::dct_run(dct->data());
                v[id].store(static_cast<double>((*dct)[0]), std::memory_order_relaxed);

                if (s->period_ns > 0) {
                    int64_t t_sched = static_cast<int64_t>(
                        s->next_release_ns - s->period_ns);
                    int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                    m.latency_ns.push_back(lat);
                    m.sum_lat += lat;
                    if (lat < m.min_lat) m.min_lat = lat;
                    if (lat > m.max_lat) m.max_lat = lat;
                    if (static_cast<uint64_t>(lat) > dl) ++m.misses;
                }
            };

        } else { // sink
            int pred = preds.at(id)[0];
            s->execute = [v, pred, s, &m, dl] {
                uint64_t t_actual = Dispatcher::monotonic_ns();

                float val = std::fabs(static_cast<float>(
                    v[pred].load(std::memory_order_relaxed))) + 1.0f;
                (void)wcet_rt::sqrt_run(val);

                if (s->period_ns > 0) {
                    int64_t t_sched = static_cast<int64_t>(
                        s->next_release_ns - s->period_ns);
                    int64_t lat = static_cast<int64_t>(t_actual) - t_sched;
                    m.latency_ns.push_back(lat);
                    m.sum_lat += lat;
                    if (lat < m.min_lat) m.min_lat = lat;
                    if (lat > m.max_lat) m.max_lat = lat;
                    if (static_cast<uint64_t>(lat) > dl) ++m.misses;
                }
            };
        }
    }

    // DAG
    DAG dag;
    for (const auto& s : subtasks) dag.add_node(s.id, nullptr);
    for (const auto& c : conns)    dag.add_edge(c.upstream, c.downstream);

    // Entradas do TeamManager — period_ns real preservado (release-guard ativo)
    std::vector<TeamManager::SubtaskEntry> entries;
    for (const auto& s : subtasks)
        entries.push_back({ s, subtask_ptrs.at(s.id).get() });

    TeamManager tm;
    tm.initialize(entries, dag);
    tm.start();

    // Tick loop — igual a example_eval.cpp: sleep absoluto no menor período,
    // notifica cada fonte quando seu período divide o tick corrente.
    uint64_t next_tick = Dispatcher::monotonic_ns() + min_p;
    for (int tick = 1; tick <= ticks; ++tick) {
        struct timespec ts;
        ts.tv_sec  = next_tick / 1'000'000'000ULL;
        ts.tv_nsec = next_tick % 1'000'000'000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);

        for (auto& [id, p] : sources)
            if (static_cast<uint64_t>(tick) % (p / min_p) == 0)
                tm.notify(id);

        next_tick += min_p;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.stop();

    // Exporta amostras brutas — mesmo formato de latency_samples.csv
    {
        std::ofstream csv("latency_heuristic_" + slugify(heuristic_name) + ".csv");
        csv << "subtask_id,period_ms,core,priority,latency_us\n";
        csv << std::fixed << std::setprecision(3);
        for (auto& [id, m] : mmap)
            for (auto lat : m.latency_ns)
                csv << id << ','
                    << (m.period_ns / 1'000'000ULL) << ','
                    << m.core << ','
                    << m.priority << ','
                    << (lat / 1000.0) << '\n';
    }

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
    std::cout << ")\n"
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
