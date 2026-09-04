# Real-time-middleware

This repository is a C++ implementation of a real-time soft scheduler. 

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
make harness
```

Builds the three programs the experiments are run with: `allocate`,
`execute_plan` and `evaluation`, whose sources live in `scripts/`
alongside the Python tooling. 

```bash
make clean
```

## Quick start

Inspect how a plan maps its subtasks onto cores, without running anything:

```bash
make allocate && ./allocate plans/deployment_plan_sat.json
```

Run the pipeline described by a plan:

```bash
sudo ./execute_plan plans/deployment_plan.json 4
```

Measure scheduling latency, jitter and deadline misses over 50 hyperperiods:

```bash
sudo ./evaluation plans/deployment_plan_lowsat.json 50
```

The full set of experiments, including the ones that produced the tables in the
article, is in **[EXPERIMENTS.md](EXPERIMENTS.md)**.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | the middleware itself: `dispatcher.hpp`, `team_manager.*`, `ring_buffer.hpp`, `component.hpp`, `dag.*`, `allocator.hpp`, `parser_json.*` |
| `plans/` | deployment plans, from 18 to 189 subtasks |
| `examples/` | small demo programs, one per building block |
| `scripts/` | the tools the experiments are run with: `allocate`, `execute_plan` and `evaluation` in C++, plus plotting, random plan generation and the saturation sweep in Python |
| `wcet_bench/` | Malardalen WCET benchmarks used as subtask bodies, the cycle-counter study that measured their WCETs. |
| `results/` | measurement data behind the article's tables, with its own README documenting provenance |

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
