# Conformidade com Huang2012 — Análise de Implementação

**Paper de referência:** Huang-Ming Huang, Christopher Gill, Chenyang Lu.
*MCFlow: a Real-time Multi-core Aware Middleware for Dependent Task Graphs.*
IEEE RTCSA 2012. DOI 10.1109/RTCSA.2012.30

---

## 1. Conformidade por seção do paper

### §II — System Model

| Requisito do paper | Implementação | Status |
|---|---|---|
| Tarefas como DAGs (vértices = subtasks, arestas = precedência) | `dag.hpp` / `dag.cpp` | ✅ |
| Subtask *initial* (sem predecessores), *intermediate*, *terminal* (sem sucessores) | `ComponentKind { SOURCE, INTERMEDIATE, SINK }` em `component.hpp` | ✅ |
| `period_ns` e `deadline_ns` por subtask | `SubtaskInfo` em `deployment_plan.hpp` | ✅ |
| Partitioned fixed-priority scheduling (thread fixada em um core) | `pthread_setaffinity_np` em `Dispatcher::loop()` | ✅ |
| Sem migração de thread entre cores | Nenhum `pthread_setaffinity_np` muda o core após o start | ✅ |
| Prioridade aplicada com `SCHED_FIFO` | `pthread_setschedparam(SCHED_FIFO)` em `Dispatcher::loop()` | ✅ |

---

### §IV — Encapsulation via Components

| Requisito do paper | Implementação | Status |
|---|---|---|
| Três categorias: `source`, `intermediate`, `sink` | `SourceComponent`, `Component`, `SinkComponent` em `component.hpp` | ✅ |
| Cada componente especifica `input_type`, `output_type`, `config_type` | Type aliases nos três templates | ✅ |
| Construtor aceita `const config_type*` | Todos os templates têm `explicit XxxComponent(const ConfigType* config)` | ✅ |
| `execute()` — núcleo de computação por job | `virtual void execute() = 0` em `ComponentBase` | ✅ |
| `preallocate()` chamado antes do loop RT para alocar memória | `virtual void preallocate() {}` em `ComponentBase` | ✅ |
| `init_input()` / `init_output()` inicializam buffers separadamente da construção | Métodos em `Component`, `SourceComponent`, `SinkComponent` | ✅ |
| Ring buffer lock-free por canal de comunicação | `RingBuffer<T,N>` em `ring_buffer.hpp` | ✅ |
| Cache-line padding em cada slot (sem false sharing) | `struct alignas(64) Slot` + `static_assert` | ✅ |
| Fórmula de tamanho: `N = next_pow2(max(2, ceil(D/T) + depth))` | `ring_buffer_n()` em `ring_buffer.hpp` | ✅ |
| Fan-in: slot com um campo por supplier + bitmask de prontidão | `MultiSupplierRingBuffer<T,N,NumSuppliers>` | ✅ |

---

### §IV-B — Interface Type Safety (Adapters)

| Requisito do paper | Implementação | Status |
|---|---|---|
| Adapter é função C/C++ que converte saída do upstream para entrada do downstream | `Adapter<UpstreamComp, DownstreamComp>` em `adapter.hpp` | ✅ |
| Suporte a lambda com captura (não apenas ponteiro de função) | `AdapterFunction = std::function<...>` | ✅ |
| Middleware aplica adapter **transparentemente** na conexão | **Não implementado** — `TeamManager::initialize()` faz wiring direto sem adapters | ⚠️ parcial |

---

### §IV-C — Deployment Plan e Code Generation

| Requisito do paper | Implementação | Status |
|---|---|---|
| Plano inclui hosts e endereços de rede | `HostInfo` em `deployment_plan.hpp` | ✅ struct |
| Plano inclui tasks, subtasks, core, priority, period, deadline, config | `SubtaskInfo`, `TaskInfo`, `DeploymentPlan` | ✅ |
| Plano inclui conexões entre subtasks | `ConnectionInfo` | ✅ |
| Parser JSON do plano | `parser_json.hpp` / `parser_json.cpp` | ✅ |
| Geração automática de código C++ + Makefile a partir do plano | `tools/codegen.cpp` — Phase 6 | ❌ pendente |

---

### §V-A — Task Management Subsystem

| Requisito do paper | Implementação | Status |
|---|---|---|
| TeamManager cria, inicializa e termina subtasks | `TeamManager` em `team_manager.hpp/cpp` | ✅ |
| State machine: CREATED→INITIALIZED→RUNNING→TERMINATING→TERMINATED | `TeamManager::State` | ✅ |
| Um `Dispatcher` por par `(core, priority)` (particionamento correto) | `TeamManager::initialize()` — `dispatchers_` indexado por `CorePrio` | ✅ |
| Wiring automático de conexões e `fan_in_total` a partir do DAG | `TeamManager::initialize()` lê `node.predecessors` e `node.successors` | ✅ |
| Exceção em subtask aciona transição para TERMINATING | `TeamManager::on_subtask_exception()` | ✅ |
| Stop em reverse-order (sinks primeiro) | `do_stop()` itera `dispatcher_order_` em reverso | ✅ |
| Protocolo de terminação em cascata (subtask para de aceitar inputs, encaminha request) | **Não implementado** — shutdown em massa sem cascata por subtask | ⚠️ simplificado |

---

### §V-B — Dispatching Subsystem

| Requisito do paper | Implementação | Status |
|---|---|---|
| Cada dispatcher tem uma fila FIFO de subtasks | `std::queue<Subtask*> queue_` | ✅ |
| Demultiplexer como padrão Reactor para eventos assíncronos | `demultiplexer.hpp` | ✅ struct |
| Linux `epoll` + `eventfd` para notificações | `epoll_create1`, `eventfd` em `Dispatcher::start()` | ✅ |
| Timer queue para controle de release periódico | `TimerQueue` (min-heap) em `dispatcher.hpp` | ✅ |
| Idle thread com menor prioridade RT (roda quando CPU está ociosa) | `Dispatcher::idle_loop()` com `SCHED_FIFO` prioridade 1 | ✅ |
| Host Manager com coordenação remota multi-host | **Não implementado** — arquitetura single-host | ❌ fora de escopo |
| Otimização transparente intra-core vs. intra-host vs. inter-host | **Não implementado** — sempre usa eventfd/epoll | ❌ fora de escopo |

---

### §V-C — Subtask Release Mechanism (protocolo de 6 passos)

| Passo | Descrição no paper | Implementação | Status |
|---|---|---|---|
| 1 | Remove subtask da fila FIFO | `queue_.front()` / `queue_.pop()` em `Dispatcher::loop()` | ✅ |
| 2 | Verifica `in_processing` (guarda leader/followers) | `s->in_processing.exchange(true)` em `process_subtask()` linha 172 | ✅ |
| 3 | Verifica se a release time expirou (subtask periódico) | `now < s->next_release_ns` em `process_subtask()` linha 177 | ✅ |
| 4a | Release não expirou → insere na timer queue; idle thread reagenda | Bloco `timer_queue_.push(...)` linhas 181-188 | ✅ |
| 4b | Release expirou → seta `in_processing`, executa | `s->execute()` linha 199 | ✅ |
| 5 | Após execução → notifica todos os downstream | `conn.dispatcher->notify(conn.subtask)` linha 203 | ✅ |
| 6 | Limpa `in_processing`, aguarda próximo evento | `s->in_processing.store(false)` linha 206 | ✅ |

---

## 2. Irregularidades e desvios

### [ALTA] Fan-in via contador atômico desconectado do ring buffer

**Paper (Fig. 7, §V-B):** O consumer é despachado quando **todos** os suppliers copiaram dados no **mesmo slot** indexado por `seq_num`. Cada entrada da fila do consumer tem um campo por supplier — a prontidão é verificada por slot de job.

**Implementação:** `Dispatcher::notify()` usa `fan_in_received` (contador atômico global no `Subtask`) sem ligação com o `seq_num` do job:

```cpp
// dispatcher.hpp:111-113
int received = s->fan_in_received.fetch_add(1) + 1;
if (received < s->fan_in_total) return;
s->fan_in_received.store(0);  // reset para o próximo job
```

O `MultiSupplierRingBuffer` (que implementa corretamente o modelo do paper com bitmask por slot) existe em `ring_buffer.hpp` mas **não está conectado** ao dispatcher.

**Risco:** Em sistemas com pipelining habilitado (múltiplos jobs em voo), a notificação do job J+1 pode chegar antes do `store(0)` do job J, resultando em dispatch prematuro ou perda de notificação. O `in_processing` flag mitiga parcialmente (um único job por vez), mas não elimina a condição.

**Correção necessária:** Conectar `MultiSupplierRingBuffer` ao mecanismo de fan-in do dispatcher, verificando `ready(seq_num)` antes de enfileirar o subtask.

---

### [MÉDIA] Protocolo de terminação simplificado

**Paper (§V-A):** Terminação em cascata iniciada pelo team manager:
1. Envia termination request a todos os subtasks downstream
2. Cada subtask para de aceitar novos inputs
3. Cada subtask encaminha o request aos seus sucessores
4. Cada subtask notifica o team manager assincronamente
5. Team manager desaloca recursos quando recebe todas as notificações

**Implementação:** `stop()` → `do_stop()` para todos os dispatchers em reverse-order diretamente. Não há mecanismo por-subtask de "parar de aceitar inputs". Subtasks em execução podem receber novas notificações até o `Dispatcher::stop()` retornar.

---

### [MÉDIA] Adapters não aplicados automaticamente pelo middleware

**Paper (§IV-B):** "MCFlow also allows adapters for component connections to be specified. An adapter is a C or C++ function that takes the output of an upstream component and converts it into the input of a downstream component."

O paper implica que o middleware aplica o adapter transparentemente na conexão, sem o desenvolvedor precisar inserir a chamada manualmente em `execute()`.

**Implementação:** `adapter.hpp` existe mas `TeamManager::initialize()` conecta subtasks diretamente via `s->downstream.push_back(...)`. O adapter, se usado, deve ser inserido manualmente na função `execute()` do componente.

---

### [BAIXA] Host Manager e comunicação multi-host ausentes

**Paper (Fig. 2):** Arquitetura completa inclui `HostManager` coordenando múltiplos `TeamManager`s em hosts distintos, com IPC entre hosts via sockets TCP/IP.

**Implementação:** Single-host only. `HostInfo` em `deployment_plan.hpp` existe como estrutura de dados mas não há camada de rede. Este é um escopo intencional para o projeto atual (Phases 0–5).

---

### [BAIXA] Geração de código incompleta

**Paper (§IV-C, contribuição #1):** "facilitates system integration and deployment through automatic code generation at compile-time from a deployment plan specification."

**Implementação:** `tools/codegen.cpp` é Phase 6 (pendente). O paper considera a geração de código a primeira e principal contribuição do MCFlow.

---

## 3. Código morto e variáveis sem uso

### A. `Demultiplexer::all_suppliers_ready()` — função nunca chamada

**Arquivo:** `demultiplexer.hpp:54-57`

```cpp
static bool all_suppliers_ready(const Subtask* s) {
    return s->fan_in_received.load(std::memory_order_acquire)
           >= s->fan_in_total;
}
```

**Diagnóstico:** A lógica de fan-in foi incorporada diretamente em `Dispatcher::notify()`. Esta função nunca é chamada em nenhum arquivo do projeto. Além disso, a implementação usa `>=` em vez de `==`, o que seria errado se `fan_in_received` pudesse ultrapassar `fan_in_total` (possível na condição de corrida descrita na irregularidade #1).

**Recomendação:** Remover. Se a intenção era expor a verificação de fan-in como API de inspeção, documentar explicitamente e testar.

---

### B. `Dispatcher::subtasks_` — vetor populado mas nunca lido

**Arquivo:** `dispatcher.hpp:339`

```cpp
std::vector<Subtask*> subtasks_;
```

`register_subtask()` (`dispatcher.hpp:101`) é chamado em `team_manager.cpp:71` para cada subtask, mas nenhum método do `Dispatcher` itera ou consulta `subtasks_`. O vetor ocupa memória e mantém referências sem nenhum uso funcional.

**Recomendação:** Remover `subtasks_` e `register_subtask()`. Se no futuro for necessário inspecionar subtasks registrados num dispatcher (e.g., para debug), readicionar com propósito claro.

---

### C. `Demultiplexer::process()` — wrapper trivial, inversão arquitetural

**Arquivo:** `demultiplexer.hpp:46-48`

```cpp
static void process(Subtask* s, Dispatcher& dispatcher) {
    dispatcher.process_subtask(s);  // delega completamente
}
```

**Diagnóstico:** O paper (Fig. 2, §V-C) descreve o `Demultiplexer` como o componente que *possui* a lógica dos 6 passos. Na implementação, os 6 passos estão em `Dispatcher::process_subtask()` e o `Demultiplexer` apenas delega. É uma inversão do papel arquitetural.

A função é chamada apenas em `tests/test_dispatcher.cpp` (um teste). No código de produção, `Dispatcher::loop()` chama `process_subtask()` diretamente.

**Recomendação:** Manter por compatibilidade de teste, mas documentar explicitamente a inversão. Alternativamente, mover os 6 passos para `Demultiplexer::process()` e ter `Dispatcher::loop()` chamá-lo, restaurando o papel arquitetural do paper.

---

### D. `main.cc` — `cfg` configurado mas não consumido pelos componentes

**Arquivo:** `main.cc`

```cpp
config_t cfg;
cfg.task_id   = 1;
cfg.core_id   = 0;
cfg.priority  = 1;
cfg.deadline  = 1000000;
```

Os campos são preenchidos mas `ComponentA` e `ComponentB` recebem o ponteiro `const config_t*` sem nunca acessar `config_` em seus `execute()`. É código de exemplo/demonstração, mas pode confundir leitores sobre se a configuração está de fato em uso.

**Recomendação:** Ou usar os campos no `execute()` dos componentes de exemplo, ou remover a configuração do exemplo para não induzir ao erro.

---

### E. `examples/example_ring.cpp` — `release(1)` antes de qualquer escrita

**Arquivo:** `examples/example_ring.cpp` (linha com `ring.release(1)` no início do loop)

`release(seq_num)` avança `consumer_pos_` para `seq_num + 1 = 2`, sinalizando que o slot 1 foi consumido antes de qualquer escrita nele. Isto é semanticamente incorreto — `release()` deve ser chamado *após* `read()`, não antes de `write()`.

**Recomendação:** Remover ou mover a chamada para após o `read()` correspondente.

---

## 4. Status de implementação por fase

| Fase | Descrição | Status |
|---|---|---|
| Phase 0 | Bug fixes iniciais | ✅ completo |
| Phase 1 | Operações DAG (topological sort, cycle detection, fan-in/out, pipeline depth) | ✅ completo |
| Phase 2 | Ring buffer lock-free com cache-line padding e backpressure | ✅ completo |
| Phase 3 | Classificação de componentes (SOURCE/INTERMEDIATE/SINK) + `preallocate()` | ✅ completo |
| Phase 4 | Dispatcher com periodicidade, fan-in, release-guard (6 passos) | ✅ completo |
| Phase 5 | TeamManager (lifecycle, auto-wiring, agrupamento por (core,priority)) | ✅ completo |
| Phase 6 | Ferramenta de geração de código a partir do deployment plan | ❌ pendente |

**Cobertura geral estimada:** ~85% das funcionalidades descritas no paper, excluindo a camada de comunicação multi-host (fora de escopo) e a geração de código (Phase 6).

---

## 5. Resumo executivo

A implementação reproduz fielmente as contribuições centrais do paper:

- **Modelo de componentes** (§IV): conforme, incluindo tipos, `preallocate()` e `init_input/output`.
- **Ring buffer** (§IV): correto em sizing, lock-free, cache-line padding e `MultiSupplierRingBuffer`.
- **Dispatcher** (§V-B, V-C): protocolo de 6 passos, timer queue, idle thread e core-pinning todos conformes.
- **TeamManager** (§V-A): state machine, agrupamento por (core,priority) e wiring automático conformes.

Os desvios mais relevantes para correção futura são:

1. **Fan-in desconectado do ring buffer** (Alta) — o contador atômico não garante consistência por `seq_num`; `MultiSupplierRingBuffer` deveria ser o mecanismo central.
2. **Adapters não aplicados transparentemente** (Média) — `adapter.hpp` existe mas o wiring no `TeamManager` não o usa.
3. **`Dispatcher::subtasks_`** — vetor morto que deve ser removido junto com `register_subtask()`.
4. **`Demultiplexer::all_suppliers_ready()`** — função morta; remover.
