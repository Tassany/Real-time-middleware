# Real-time-middleware

A C++17 reimplementation of **MCFlow**, a real-time middleware that runs
dependent task graphs on multi-core platforms, extended with two things the
original paper does not provide: configurable bin-packing allocation of
subtasks to cores, and a measurement harness that reports per-job scheduling
latency, jitter and deadline misses under `SCHED_FIFO` on Linux `PREEMPT_RT`.

A deployment plan is a JSON file describing tasks, their subtasks, periods,
priorities and connections. The middleware parses it, assigns each subtask to a
core (either as written in the plan, or automatically through a bin-packing
heuristic), builds one dispatcher thread per core, and runs the graph.

Reference paper: Huang-Ming Huang, Christopher Gill, Chenyang Lu. *MCFlow: a
Real-time Multi-core Aware Middleware for Dependent Task Graphs.* IEEE RTCSA
2012. The paper itself is not redistributed here; see `NOTICE.md`.

## Requirements

- Linux, ideally with `PREEMPT_RT`. Everything builds and runs on a stock
  kernel, but the latency numbers are only meaningful on `PREEMPT_RT`.
- `g++` with C++17 support, GNU `make`, and `objcopy` (GNU binutils) for the
  benchmark objects.
- Root access, to obtain `SCHED_FIFO`. Without it the runtime prints
  `RT priority not applied` and keeps going under the default scheduler.
- Python 3 with `pandas` and `matplotlib`, for the scripts in `scripts/`.

The only C++ dependency, `nlohmann/json` v3.12.0, is vendored in
`include/nlohmann/json.hpp`, so no network access is needed to build.

## Build and test

```bash
make examples
```

```bash
make test
```

`make test` builds and runs the six suites covering phases 0 to 5 (parser, DAG,
ring buffer, component model, dispatcher, team manager): 35 assertions, all of
which pass without root.

```bash
make clean
```

## Quick start

Inspect how a plan maps its subtasks onto cores, without running anything:

```bash
make show_alloc && ./show_alloc plans/deployment_plan_sat.json
```

Run the pipeline described by a plan:

```bash
sudo ./example_from_plan plans/deployment_plan.json 4
```

Measure scheduling latency, jitter and deadline misses over 50 hyperperiods:

```bash
sudo ./example_eval plans/deployment_plan_lowsat.json 50
```

The full set of experiments, including the ones that produced the tables in the
article, is in **[EXPERIMENTS.md](EXPERIMENTS.md)**.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the middleware itself: `dispatcher.hpp`, `team_manager.*`, `ring_buffer.hpp`, `component.hpp`, `dag.*`, `allocator.hpp`, `parser_json.*` |
| `plans/` | deployment plans, from 18 to 189 subtasks |
| `examples/` | runnable programs, including the `example_eval` measurement harness |
| `tests/` | the six test suites run by `make test` |
| `scripts/` | Python tooling: plotting, random plan generation, saturation sweep |
| `wcet_bench/` | Malardalen WCET benchmarks used as subtask bodies, the cycle-counter study that measured their WCETs, and the code generator of phase 6 (`tools/codegen.cpp`, work in progress) |
| `results/` | measurement data behind the article's tables, with its own README documenting provenance |
| `docs/` | architecture, design notes, conformance analysis against the paper |

## Documentation

- [docs/visao-arquitetural.md](docs/visao-arquitetural.md) — architectural overview
- [docs/layers-deep-dive.md](docs/layers-deep-dive.md) — configuration, scheduling and orchestration layers in detail
- [docs/dispatcher.md](docs/dispatcher.md), [docs/team_manager.md](docs/team_manager.md) — the two core components
- [docs/conformidade-huang2012.md](docs/conformidade-huang2012.md) — where this implementation departs from the paper
- [docs/report_eval_rpi5.md](docs/report_eval_rpi5.md) — evaluation report on Raspberry Pi 5

## Citing

If you use this software, please cite it using the metadata in
[CITATION.cff](CITATION.cff). Authors and their roles are listed in
[AUTHORS](AUTHORS).

<!-- TODO: after archiving (see below), paste the SWHID here, e.g.
     swh:1:rev:<hash>;origin=https://github.com/Tassany/Real-time-middleware;visit=swh:1:snp:<hash>
-->

## Archiving this repository

To deposit a citable snapshot in [Software Heritage](https://www.softwareheritage.org/):

1. Push the state you want archived.
2. Submit `https://github.com/Tassany/Real-time-middleware.git` at
   <https://archive.softwareheritage.org/browse/origin/save/>, choosing `git` as
   the origin type.
3. Once the visit completes, browse to the archived revision and copy its SWHID
   from the permalink box, keeping the `origin=` and `visit=` qualifiers.
4. Paste that SWHID in the section above and in the article. In LaTeX, the
   `\swhurl`/`\swhref` macros turn a SWHID into a clickable link, and the
   `path=` and `lines=` qualifiers let a caption point at an exact code
   fragment. The `biblatex-software` package produces the bibliography entry.

## License

MIT, see [LICENSE](LICENSE). Third-party material redistributed here, and the
terms attached to it, is documented in [NOTICE.md](NOTICE.md).

## Contact

Tassany Onofre de Oliveira — <tassany.onofre-de-oliveira@enac.fr>
