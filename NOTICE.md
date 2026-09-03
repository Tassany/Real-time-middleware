# Third-party material

This repository is distributed under the MIT License (see `LICENSE`), which
covers the code written for this project: `src/`, `examples/`,
`scripts/` and the deployment plans in `plans/`. The items below come
from elsewhere and keep their own terms.

## Malardalen WCET benchmarks — `wcet_bench/*.c`

The 37 C programs in `wcet_bench/` are from the Malardalen WCET Benchmark
Suite, collected by the WCET project at Malardalen Real-Time Research Centre
(MRTC) from earlier academic sources. Individual origins are recorded in the
headers of the files themselves, which were kept unmodified: `matmult.c`, for
instance, still carries its RCS `$Id$` line and the note describing the
adaptation by Thomas Lundqvist at Chalmers.

The suite is published for research use and its files carry heterogeneous
provenance rather than a single license header. They are redistributed here
unmodified so that the WCET figures in the deployment plans can be reproduced.
Anyone reusing them outside a research context should check the terms with
MRTC.

Six of them are compiled into subtask bodies by the `Makefile`: `matmult`,
`bsort100`, `crc`, `ud`, `fft1` and `statemate`. The remaining files are kept
for future use. `wcet_bench/tools/codegen.cpp` and
`wcet_bench/cycle_counter/` are original work of this project, not part of the
suite.

## nlohmann/json — `include/nlohmann/json.hpp`

Single-header JSON library, version 3.12.0, by Niels Lohmann, MIT License.
Copyright (c) 2013-2025 Niels Lohmann, declared by the `SPDX-License-Identifier`
line at the top of the header file. It is vendored rather than downloaded so that an
archived snapshot of this repository builds without network access, and so that
the dependency version is pinned rather than whatever `latest` happens to be.

Upstream: <https://github.com/nlohmann/json>

## Reference papers — not redistributed

The two papers this work builds on are cited but **not** included in the
repository, because they are copyrighted by their publishers:

- Huang-Ming Huang, Christopher Gill, Chenyang Lu. *MCFlow: a Real-time
  Multi-core Aware Middleware for Dependent Task Graphs.* 2012 IEEE
  International Conference on Embedded and Real-Time Computing Systems and
  Applications (RTCSA), 2012. — the system reimplemented here.
- M. Verucchi et al. — real-time DAG scheduling, cited in the accompanying
  article.

`*.pdf` is in `.gitignore` so that local copies of these papers are not
committed by accident.
