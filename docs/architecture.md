# MCFlow — Architecture Reference

> Based on: Huang et al., *"MCFlow: A Real-Time Streaming Framework for Multi-Core Platforms"*, IEEE RTCSA 2012.

---

## 1. System Overview

MCFlow is a **real-time streaming middleware** for multi-core systems. Its goal is to execute a directed acyclic graph (DAG) of periodic tasks while respecting hard deadlines, minimising scheduling jitter, and avoiding dynamic memory allocation at runtime.

The key design decisions are:

- **Partitioned fixed-priority scheduling** — each subtask is pinned to one CPU core and assigned a POSIX `SCHED_FIFO` priority. Subtasks on the same `(core, priority)` share exactly one dispatcher thread.
- **Lock-free ring buffers** — data flows between subtasks through cache-line-padded, sequence-number-addressed ring buffers to eliminate false sharing and reduce latency.
- **Event-driven dispatch** — dispatchers block on `epoll`/`eventfd` instead of spinning, so idle cores consume no CPU.
- **Static allocation** — all buffers and queues are pre-sized from the deployment plan before `start()` is called.

---

## 2. Layered Architecture

```mermaid
block-beta
  columns 1

  block:app["Application Layer"]
    A["main thread\n(tick loop — fires sources)"]
  end

  block:mgmt["Management Layer"]
    B["TeamManager\n(lifecycle · wiring · dispatcher grouping)"]
  end

  block:sched["Scheduling Layer"]
    C["Dispatcher\n(SCHED_FIFO thread + idle thread\nper (core,priority) pair)"]
  end

  block:exec["Execution Layer"]
    D["Subtask\n(id · period_ns · execute() · downstream wiring)"]
  end

  block:comp["Component Layer"]
    E["ComponentBase / Component<I,O,C>\nSourceComponent / SinkComponent"]
  end

  block:data["Data Layer"]
    F["RingBuffer<T,N>  ·  MultiSupplierRingBuffer<T,N,S>"]
  end

  block:cfg["Configuration Layer"]
    G["DeploymentPlan (JSON)\nJsonParser  ·  DAG"]
  end

  A --> B
  B --> C
  C --> D
  D --> E
  D --> F
  G --> B
```

---

## 3. File Map

| File | Layer | Responsibility |
|---|---|---|
| `deployment_plan.hpp` | Configuration | Plain structs: `DeploymentPlan`, `TaskInfo`, `SubtaskInfo`, `ConnectionInfo` |
| `parser_json.hpp/.cpp` | Configuration | Reads JSON into `DeploymentPlan` using nlohmann/json |
| `dag.hpp/.cpp` | Configuration | Directed acyclic graph; topological sort (Kahn); pipeline depth; cycle detection |
| `component.hpp` | Component | Abstract `ComponentBase`; templates `Component<I,O,C>`, `SourceComponent<O,C>`, `SinkComponent<I,C>` |
| `adapter.hpp` | Component | Type-safe `Adapter<Up,Down>` for converting between mismatched output/input types |
| `ring_buffer.hpp` | Data | `RingBuffer<T,N>` (SPSC) and `MultiSupplierRingBuffer<T,N,S>` (fan-in) |
| `dispatcher.hpp` | Scheduling | `Subtask` struct; `Dispatcher` class; 6-step release-guard protocol |
| `team_manager.hpp/.cpp` | Management | Groups subtasks into dispatchers; wires DAG edges; manages lifecycle |

---

## 4. Component Model

Every user-defined processing node inherits from one of these three templates. The templates enforce type safety at compile time through `input_type` / `output_type` aliases.

```mermaid
classDiagram
    class ComponentBase {
        <<abstract>>
        +preallocate() void
        +execute() void*
        +kind() ComponentKind
    }

    class Component {
        <<template I,O,C>>
        +input_  : I
        +output_ : O
        #config_ : C*
        +execute() void*
    }

    class SourceComponent {
        <<template O,C>>
        +output_ : O
        #config_ : C*
        +execute() void*
        +kind() SOURCE
    }

    class SinkComponent {
        <<template I,C>>
        +input_ : I
        #config_ : C*
        +execute() void*
        +kind() SINK
    }

    ComponentBase <|-- Component
    ComponentBase <|-- SourceComponent
    ComponentBase <|-- SinkComponent
```

The `Adapter<Up,Down>` class sits between two components and converts `Up::output_type` into `Down::input_type` using a user-supplied `std::function`. It is instantiated at build time (Phase 6 / codegen) and is not yet wired automatically by `TeamManager`.

---

## 5. Deployment Plan

A `DeploymentPlan` is loaded from JSON and describes the complete graph of tasks. The three core structs map directly to the JSON keys:

```mermaid
classDiagram
    class DeploymentPlan {
        +hosts       : HostInfo[]
        +tasks       : TaskInfo[]
        +connections : ConnectionInfo[]
    }
    class TaskInfo {
        +id       : int
        +subtasks : SubtaskInfo[]
    }
    class SubtaskInfo {
        +id             : int
        +component_type : string
        +host           : string
        +core           : int
        +priority       : int
        +period_ns      : uint64
        +deadline_ns    : uint64
        +config         : json
    }
    class ConnectionInfo {
        +upstream   : int
        +downstream : int
    }

    DeploymentPlan "1" *-- "N" TaskInfo
    DeploymentPlan "1" *-- "N" ConnectionInfo
    TaskInfo       "1" *-- "N" SubtaskInfo
```

### Example: `plans/deployment_plan.json` topology

Six independent pipelines, each with period assigned by Rate-Monotonic Scheduling (shorter period → higher priority):

```mermaid
graph LR
    subgraph T1["Task 1 — 1 ms · core 0 · prio 16"]
        S1(source 1) --> I2(interm 2) --> K3(sink 3)
    end
    subgraph T2["Task 2 — 2 ms · core 1 · prio 14"]
        S4(source 4) --> I5(interm 5) --> K6(sink 6)
    end
    subgraph T3["Task 3 — 6 ms · core 2 · prio 12"]
        S7(source 7) --> I8(interm 8) --> K9(sink 9)
    end
    subgraph T4["Task 4 — 12 ms · core 3 · prio 10"]
        S10(source 10) --> I11(interm 11) --> K12(sink 12)
    end
    subgraph T5["Task 5 — 24 ms · core 4 · prio 8"]
        S13(source 13) --> I14(interm 14) --> K15(sink 15)
    end
    subgraph T6["Task 6 — 48 ms · core 5 · prio 6"]
        S16(source 16) --> I17(interm 17) --> K18(sink 18)
    end
```

---

## 6. DAG

`DAG` stores nodes as adjacency lists (`predecessors` + `successors`) and provides three algorithms consumed by `TeamManager::initialize()`:

| Method | Algorithm | Used for |
|---|---|---|
| `topological_sort()` | Kahn's BFS | Dispatcher creation order; ensures sources are started before sinks |
| `pipeline_depth()` | Longest path (DP on topo order) | Ring buffer sizing formula |
| `fan_in_count(id)` | Predecessor list size | Sets `Subtask::fan_in_total` |

---

## 7. TeamManager

`TeamManager` is the single orchestration point. It owns all `Dispatcher` instances and mediates between the application and the scheduling layer.

### 7.1 Lifecycle State Machine

```mermaid
stateDiagram-v2
    [*] --> CREATED : constructor

    CREATED --> INITIALIZED : initialize(entries, dag)
    note right of INITIALIZED
        Dispatchers created
        DAG edges wired
        period_ns set
        ring_buffer_sizes computed
    end note

    INITIALIZED --> RUNNING : start()
    note right of RUNNING
        One SCHED_FIFO thread
        per (core,priority) pair
        is now active
    end note

    RUNNING --> TERMINATING : stop() or subtask exception
    TERMINATING --> TERMINATED : do_stop() joins all threads

    TERMINATED --> [*]
```

### 7.2 Dispatcher Grouping

The critical rule from paper Section V-B: **one `Dispatcher` per `(core, priority)` pair**, not per subtask. Multiple subtasks on the same core/priority share one thread, one `eventfd`, and one ready queue.

```mermaid
graph TD
    subgraph entries["SubtaskEntry list (from deployment plan)"]
        E1["id=1  core=0 prio=16"]
        E2["id=2  core=0 prio=16"]
        E3["id=3  core=0 prio=16"]
        E4["id=4  core=1 prio=14"]
        E5["id=5  core=1 prio=14"]
    end

    subgraph dispatchers["Dispatchers created"]
        D1["Dispatcher\ncore=0 prio=16"]
        D2["Dispatcher\ncore=1 prio=14"]
    end

    E1 --> D1
    E2 --> D1
    E3 --> D1
    E4 --> D2
    E5 --> D2
```

### 7.3 Wiring (done inside `initialize()`)

For every DAG edge `upstream → downstream`, `TeamManager` appends a `SubtaskConn{dispatcher_of_downstream, ptr_to_downstream}` into `upstream->downstream`. After `execute()` finishes, the `Dispatcher` iterates this list and calls `dispatcher->notify(subtask)` for each entry — this is how data-driven activation propagates through the pipeline automatically.

---

## 8. Dispatcher

Each `Dispatcher` owns two POSIX threads pinned to the same CPU core:

```mermaid
graph TB
    subgraph Dispatcher["Dispatcher (core C, priority P)"]
        direction TB

        subgraph MainThread["Main thread — SCHED_FIFO prio P"]
            EP["epoll_wait(efd)"]
            DQ["dequeue Subtask* from queue_"]
            PS["process_subtask()"]
            EP --> DQ --> PS --> EP
        end

        subgraph IdleThread["Idle thread — SCHED_FIFO prio 1"]
            IEP["epoll_wait(idle_efd) timeout=10ms"]
            TQ["dispatch_expired_timers()"]
            IEP --> TQ --> IEP
        end

        subgraph Queues["Internal state"]
            Q["queue_\n(ready subtasks)"]
            TM["timer_queue_\n(deferred — min-heap by release_ns)"]
            EFD["eventfd efd"]
            IEFD["eventfd idle_efd"]
        end

        PS -- "defer (too early)" --> TM
        TM -- "re-enqueue when ready" --> Q
        Q --> DQ
        EFD -. "wakes" .-> EP
        IEFD -. "wakes" .-> IEP
        TQ -- "writes to efd" --> EFD
        PS -- "writes to idle_efd" --> IEFD
    end

    EXT["notify() called\nfrom any thread"] -- "fan_in gate\n→ push to queue_\n→ write efd" --> Q
```

### 8.1 The 6-Step Release-Guard Protocol (`process_subtask`)

This is the heart of the real-time scheduling correctness. It prevents double execution and enforces strict periodicity.

```mermaid
flowchart TD
    A["process_subtask(s) called"] --> B

    B{"Step 2\ns->in_processing\n.exchange(true)"}
    B -- "was already true\n(already running)" --> Z["return — skip"]
    B -- "was false\n(we own it)" --> C

    C{"Steps 3+4a\nperiod_ns > 0 AND\nnow < next_release_ns?"}
    C -- "YES — too early" --> D["push to timer_queue_\nrelease in_processing\nwake idle thread"]
    C -- "NO — ready to run" --> E

    E["Step 4b\nnext_release_ns += period_ns\n(or set = now+T on first job)"]
    E --> F["s->execute()"]
    F --> G["Step 5\nfor each conn in s->downstream:\n  conn.dispatcher->notify(conn.subtask)"]
    G --> H["Step 6\nin_processing.store(false)"]
    H --> I["done"]
```

**Key invariant exposed by Step 4b:** when `execute()` runs, `s->next_release_ns` already holds the *next* release time. Therefore, inside `execute()`:

```
t_scheduled = s->next_release_ns - s->period_ns   // release of the current job
t_actual    = Dispatcher::monotonic_ns()           // wall clock at execute() entry
latency     = t_actual - t_scheduled
```

---

## 9. Subtask

`Subtask` is the runtime unit of work. It carries both scheduling metadata and the callable:

```mermaid
classDiagram
    class Subtask {
        +id               : int
        +execute          : function~void~
        +period_ns        : uint64
        +next_release_ns  : uint64
        +in_processing    : atomic~bool~
        +fan_in_total     : int
        +fan_in_received  : atomic~int~
        +downstream       : SubtaskConn[]
    }
    class SubtaskConn {
        +dispatcher : Dispatcher*
        +subtask    : Subtask*
    }
    Subtask "1" *-- "N" SubtaskConn
```

`Subtask` is **non-copyable** because `std::atomic` members cannot be copied. This requires all `Subtask` objects to be heap-allocated via `std::make_unique` and referred to by raw pointer in lambdas and dispatcher queues.

---

## 10. Ring Buffers

Two ring buffer types handle inter-subtask data flow. Both use cache-line alignment to prevent false sharing.

```mermaid
classDiagram
    class RingBuffer {
        <<template T, N>>
        -slots_       : Slot[N]
        -consumer_pos_: atomic~size_t~
        +write(seq_num, value)
        +read(seq_num) T&
        +release(seq_num)
    }
    note for RingBuffer "SPSC — single producer, single consumer\nN must be power of 2"

    class MultiSupplierRingBuffer {
        <<template T, N, NumSuppliers>>
        -slots_       : Slot[N]
        -consumer_pos_: atomic~size_t~
        +write(seq_num, supplier_id, value)
        +ready(seq_num) bool
        +read(seq_num, supplier_id) T&
        +release(seq_num)
    }
    note for MultiSupplierRingBuffer "MPSC fan-in — each slot has one field\nper supplier + atomic bitmask"
```

### Sizing formula (paper Section IV)

```
N = next_power_of_2( max(2,  ceil(deadline_downstream / period_upstream)  +  pipeline_depth) )
```

- `ceil(D/T)` — how many upstream jobs can fire before the downstream deadline; these are "in-flight" simultaneously.
- `pipeline_depth` — worst-case number of stages concurrently holding a buffer slot.
- `next_power_of_2` — required so index wrapping uses a bitmask (`seq & (N-1)`) instead of modulo.

---

## 11. Full Runtime Sequence

From a `notify()` call on the main thread to `execute()` running on a dispatcher thread:

```mermaid
sequenceDiagram
    participant Main as Main Thread
    participant TM as TeamManager
    participant D0 as Dispatcher (core 0)
    participant S1 as Subtask 1 (source)
    participant D1 as Dispatcher (core 1)
    participant S2 as Subtask 2 (intermediate)

    Main->>TM: notify(subtask_id=1)
    TM->>D0: notify(s1)
    D0->>D0: fan_in gate (fan_in_received++ == fan_in_total?)
    D0->>D0: push s1 to queue_
    D0->>D0: write(efd, 1)
    D0->>D0: [epoll wakes main thread]
    D0->>D0: dequeue s1
    D0->>D0: process_subtask(s1)
    Note over D0: Step 2: set in_processing
    Note over D0: Step 4b: next_release_ns += period_ns
    D0->>S1: s1->execute()
    S1-->>D0: return
    Note over D0: Step 5: notify downstream
    D0->>D1: notify(s2)
    D1->>D1: push s2 to queue_
    D1->>D1: write(efd, 1)
    Note over D0: Step 6: clear in_processing
    D1->>D1: dequeue s2
    D1->>D1: process_subtask(s2)
    D1->>S2: s2->execute()
    S2-->>D1: return
    Note over D1: propagate further downstream...
```

---

## 12. Known Deviations from the Paper

| # | Severity | Description |
|---|---|---|
| 1 | HIGH | Fan-in uses `atomic<int> fan_in_received` instead of `MultiSupplierRingBuffer` indexed by `seq_num`. The counter resets when it reaches `fan_in_total`, which is correct for non-overlapping jobs but loses per-job ordering in deep pipelines with overlapping jobs. |
| 2 | MED | `Adapter` objects exist but are not applied automatically by `TeamManager`. Connections between mismatched types require manual wiring. |
| 3 | LOW | Shutdown uses bulk `stop()` (sinks-first reverse order) rather than a per-subtask cascade as described in Section V-D. |
| 4 | LOW | Single-host only. The paper's Host Manager and network transport layer are not implemented. |
| 5 | INFO | `Dispatcher::subtasks_` + `register_subtask()` are populated but never read. Dead code. |
| 6 | INFO | `Demultiplexer::all_suppliers_ready()` is defined but never called. Dead code. |
