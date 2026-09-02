# MCFlow — Análise do Paper e Plano de Implementação

> Referência: Huang-Ming Huang, Christopher Gill, Chenyang Lu.  
> *MCFlow: a Real-time Multi-core Aware Middleware for Dependent Task Graphs*  
> IEEE RTCSA 2012.

---

## 1. Síntese do Paper

### Motivação

RT-CORBA/TAO não foi projetado para paralelismo fino de subtarefas em plataformas multi-core: sofre com marshaling desnecessário entre cores do mesmo host, migração de threads e sobrecarga de sincronização crescente com o número de cores. MCFlow nasce para preencher esse espaço.

### Modelo de Sistema

- ~~Ambiente distribuído: hosts multi-core conectados por rede~~ **→ fora de escopo: implementação restrita a um único host multi-core**
- Tarefas modeladas como **DAGs**: vértices = subtarefas, arestas = dependências de precedência
- **Team**: conjunto de subtarefas de uma mesma tarefa alocadas no mesmo host
- **Job**: uma invocação de uma tarefa; **Subjob**: uma invocação de uma subtarefa
- Cada job possui: release time `t^r_{i,k}` e deadline relativo `D_{i,k}`; cada subjob tem release time individual
- Escalonamento **particionado com prioridade fixa**: threads pinadas a cores específicos, sem migração
- Período, deadline e prioridade calculados **offline** e codificados no deployment plan

### Três Contribuições Principais

| # | Contribuição | Descrição | Escopo |
|---|---|---|---|
| 1 | **Modelo de Componente Leve** | Cada componente C++ declara `input_type`, `output_type`, `config_type`, `execute()`, `init_input()`, `init_output()`; ferramenta de deployment gera C++ + Makefiles automaticamente | ✅ implementado |
| 2 | **Otimização Transparente de Comunicação** | Intra-core: chamada direta; inter-core: ring buffer lock-free; ~~inter-host: sockets~~ | ⚠️ apenas intra-host |
| 3 | **Polimorfismo de Interface via Adapters** | Adapters type-safe separam correção funcional das restrições de cópia de memória | ✅ implementado |

### Arquitetura — Dois Subsistemas (Figura 2 do paper)

#### Subsistema de Gerenciamento de Tarefas

| Componente | Responsabilidade | Escopo |
|---|---|---|
| ~~**Host Manager**~~ | ~~Coordena todos os Team Managers no host; protocolo de inicialização distribuído~~ | ❌ fora de escopo (single-host) |
| **Team Manager** | Ciclo de vida de um conjunto de subtarefas: CREATED → INITIALIZED → RUNNING → TERMINATING → TERMINATED | ✅ implementado |
| Protocolo de terminação | Executado na prioridade RT da tarefa; propaga `termination request` para sucessores; desaloca recursos após confirmação de todos | ✅ implementado |

#### Subsistema de Despacho (Real-time)

| Componente | Responsabilidade |
|---|---|
| **Dispatcher** | Thread pinada a um core/prioridade via SCHED_FIFO; fila FIFO de subtarefas + **timer queue** para releases periódicos |
| **Demultiplexer** | Padrão Reactor com epoll; desmultiplexa eventos (rede, eventfd) para o Dispatcher |
| **Leader/Followers** | Opção de múltiplas threads aguardando eventos no mesmo epoll (útil quando subtarefas bloqueiam em syscalls) |
| **Idle Thread** | Prioridade mínima por CPU; executa subtarefa com menor expiration na timer queue quando CPU fica ociosa |

#### Mecanismo ITC — Inter-Thread Communication (Figura 6 do paper)

- Cada consumidor tem **fila de input própria tipada** (sem fila compartilhada)
- Cada supplier tem **fila de output própria tipada**
- **Ring buffer lock-free** com sequence numbers; sem marshaling/demarshaling intra-host
- **Padding de cache-line** em cada slot do buffer (evita false sharing)
- **Fan-in**: consumidor despachado somente quando **todos** os suppliers copiaram seus dados; cada slot do consumer queue tem um campo por supplier (Figura 7 do paper)
- Tamanho do ring buffer calculado automaticamente: `max(2, ceil(deadline/period) + pipeline_depth)`

#### Release-Guard Protocol (Seção V-C do paper)

Quando uma subtarefa termina:
1. Copia output para filas de input dos sucessores imediatos
2. Insere sucessores nas filas de subtarefa dos Dispatchers correspondentes
3. Envia notificação assíncrona via eventfd

Ao receber notificação, o Demultiplexer executa:
1. Remove subtarefa da fila
2. Verifica flag `in-processing` (atômica) — previne execução dupla no padrão leader/followers
3. Verifica se é periódica e se o release time expirou
4. Se sim: seta `in-processing`, executa; senão: insere na timer queue
5. Após execução: verifica se há mais inputs; repete até não haver
6. Limpa `in-processing`

O protocolo garante que o **intervalo entre releases nunca é menor que o período da subtarefa**.

---

## 2. Gap Analysis — Paper vs. Implementação Atual

### Legenda de Status

| Símbolo | Significado |
|---|---|
| ✅ Implementado | Funcional e cobrindo o conceito descrito no paper |
| ⚠️ Parcial | Código existe mas incompleto, com bug, ou faltando aspectos essenciais |
| ❌ Ausente | Nenhuma implementação |

---

### 2.1 Modelo de Componente (`component.hpp`, `adapter.hpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| `input_type`, `output_type`, `config_type` via templates | ✅ Implementado | `component.hpp` | `Component<I,O,C>` |
| `execute()` como interface principal | ✅ Implementado | `component.hpp` | Virtual puro em `ComponentBase` |
| `init_input()` / `init_output()` separados do construtor | ✅ Implementado | `component.hpp` | |
| Construtor aceitando ponteiro para `config_type` | ✅ Implementado | `component.hpp` | `const ConfigType*` |
| Adapter type-safe de conversão entre tipos | ✅ Implementado | `adapter.hpp` | Funcional |
| Classificação: source / intermediate / sink | ❌ Ausente | — | Existe apenas uma classe genérica |
| Adapter com lambda capturando variáveis (closure) | ⚠️ Parcial | `adapter.hpp` | Só aceita ponteiros de função raw; `std::function` não usado |
| Seleção parcial de sub-campos do output (ports) | ❌ Ausente | — | Adapter converte o tipo inteiro; sem granularidade de campo |
| Pré-alocação de memória em `init_*` (sem `new` em tempo real) | ❌ Ausente | — | Framework não oferece mecanismo de reserva antecipada |

---

### 2.2 Deployment Plan e Geração de Código (`deployment_plan.hpp`, `parser_json.cpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| Hosts e endereços de rede | ✅ Implementado | `deployment_plan.hpp` | `HostInfo` |
| Tarefas, subtarefas, conexões | ✅ Implementado | `deployment_plan.hpp`, `parser_json.cpp` | |
| Prioridade por subtarefa | ✅ Implementado | `deployment_plan.hpp` | Campo `priority` em `SubtaskInfo` |
| `period_ns` e `deadline_ns` por subtarefa | ⚠️ Parcial | `parser_json.cpp:53-54` | **BUG CRÍTICO**: referenciados no parser mas ausentes em `SubtaskInfo`; projeto não compila |
| Valores de `config_type` por subtarefa no plano | ❌ Ausente | — | Não modelado nem parseado |
| Ferramenta de geração automática de C++ + Makefile | ❌ Ausente | — | Contribuição central do paper; não existe |

---

### 2.3 DAG de Tarefas (`dag.hpp`, `dag.cpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| Nó com id, predecessores e sucessores | ✅ Implementado | `dag.hpp`, `dag.cpp` | |
| Arestas de dependência (`add_edge`) | ✅ Implementado | `dag.cpp` | |
| Fan-in / fan-out (estrutura de dados) | ⚠️ Parcial | `dag.hpp` | Estrutura suporta; dispatcher não implementa a semântica |
| Travessia topológica | ❌ Ausente | — | Necessário para inicialização e ordem de dispatch |
| Detecção de ciclos | ❌ Ausente | — | "DAG" pressupõe aciclicidade mas não é verificada |
| Cálculo de profundidade do pipeline | ❌ Ausente | — | Necessário para dimensionar ring buffers |

---

### 2.4 Ring Buffer / ITC (`ring_buffer.hpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| Lock-free com sequence numbers | ✅ Implementado | `ring_buffer.hpp` | Base sólida |
| Tamanho > profundidade do pipeline | ⚠️ Parcial | `ring_buffer.hpp` | Constraint documentada; não calculada automaticamente |
| Padding de cache-line por slot (false sharing) | ❌ Ausente | — | Paper exige explicitamente; não implementado |
| `release()` funcional com backpressure | ❌ Ausente | `ring_buffer.hpp` | No-op atual; produtor pode ultrapassar consumidor |
| Múltiplos campos por slot (fan-in de N suppliers) | ❌ Ausente | — | Paper Fig. 7: um campo por supplier; não existe |
| Cálculo automático de tamanho (deadline + período + depth) | ❌ Ausente | — | Requer deployment tool |

---

### 2.5 Dispatcher e Despacho Real-time (`dispatcher.hpp`, `dispatcher.cpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| Thread pinada a core com `pthread_setaffinity_np` | ✅ Implementado | `dispatcher.hpp` | |
| Prioridade SCHED_FIFO com `pthread_setschedparam` | ✅ Implementado | `dispatcher.hpp` | |
| Fila FIFO de subtarefas | ✅ Implementado | `dispatcher.hpp` | Queue com mutex |
| eventfd + epoll para notificação assíncrona | ✅ Implementado | `dispatcher.hpp` | EFD_SEMAPHORE + epoll_wait |
| **Timer queue** para release periódico | ❌ Ausente | — | Central ao paper; completamente ausente |
| **Release-guard protocol** (intervalo ≥ período) | ❌ Ausente | — | Sem noção de período ou release time |
| Flag `in-processing` atômica por subtarefa | ❌ Ausente | — | Necessária para evitar execução dupla |
| Padrão **leader/followers** (N threads por Dispatcher) | ❌ Ausente | — | Thread única por Dispatcher |
| **Demultiplexer** separado do Dispatcher (Reactor) | ❌ Ausente | — | Dispatcher absorve o papel do Demultiplexer |
| **Priority lanes** (N prioridades por core) | ❌ Ausente | — | Um Dispatcher = uma prioridade; múltiplas não gerenciadas |
| **Idle thread** por CPU (execução antecipada) | ❌ Ausente | — | Não existe |
| Cópia automática de output → enfileirar downstream → notificar | ❌ Ausente | — | Feito manualmente pelo usuário hoje |
| Condição de fan-in no dispatch | ❌ Ausente | — | Dispatcher não espera todos os suppliers |

---

### 2.6 Team Manager / Host Manager (`team_manager.hpp`)

| Conceito do Paper | Status | Arquivo | Observação |
|---|---|---|---|
| Enum `TeamState` com os 5 estados | ✅ Implementado | `team_manager.hpp` | Estados corretos |
| Classe `TeamManager` com ciclo de vida | ❌ Ausente | `team_manager.hpp` | Apenas o enum existe |
| `HostManager` coordenando múltiplos teams | ❌ Ausente | — | Nem o header existe |
| Protocolo de inicialização distribuído (ACK de downstream) | ❌ Ausente | — | |
| Protocolo de terminação com propagação e dealloc | ❌ Ausente | — | |
| Recovery por exceção em subtarefa | ❌ Ausente | — | |

---

### 2.7 Comunicação Inter-Host — Fora de Escopo

A implementação é restrita a **um único host multi-core**. Os mecanismos de comunicação distribuída descritos no paper não são implementados e não constam no roadmap.

| Conceito do Paper | Decisão |
|---|---|
| Comunicação via sockets TCP entre hosts | 🚫 fora de escopo |
| `HostManager` coordenando múltiplos hosts | 🚫 fora de escopo |
| Seleção automática intra-core / inter-core / inter-host | 🚫 fora de escopo (apenas inter-core via ring buffer) |
| Entrega direta ao core de destino (sem thread intermediária) | 🚫 fora de escopo |

---

### 2.8 Resumo Quantitativo (escopo single-host)

> Itens marcados como 🚫 fora de escopo são excluídos do denominador.

| Categoria | ✅ Implementado | ⚠️ Parcial | ❌ Pendente | 🚫 Fora de escopo | Total no escopo |
|---|---|---|---|---|---|
| Modelo de Componente | 7 | 0 | 1 | 0 | 8 |
| Deployment Plan | 4 | 0 | 1 | 0 | 5 |
| DAG | 5 | 0 | 0 | 0 | 5 |
| Ring Buffer / ITC | 5 | 0 | 1 | 0 | 6 |
| Dispatcher / Despacho | 9 | 0 | 1 | 0 | 10 |
| Team Manager | 6 | 0 | 0 | 0 | 6 |
| Comunicação Inter-Host | — | — | — | 4 | — |
| **TOTAL** | **36 (90%)** | **0** | **4 (10%)** | **4** | **40** |

### Lacunas Pendentes (escopo single-host)

1. **Codegen** — ferramenta `tools/codegen.cpp` que gera C++ + Makefile a partir do `plans/deployment_plan.json` (Fase 7)
2. **`preallocate()` forçado** — framework não garante que seja chamado antes do loop real-time; depende do usuário
3. **Seleção intra-core vs inter-core** — ring buffer usado para todas as conexões inter-thread; chamada direta para intra-core não está automatizada
4. **`static_assert` de N** — ring buffer não verifica em compile time que `N ≥ pipeline_depth`

---

## 3. Plano de Implementação Faseado

### Visão Geral das Fases

```
Fase 0 → Fase 1 → Fase 2 → Fase 3 → Fase 4 → Fase 5 → Fase 6
 Bug fix   DAG     RingBuf  Compon.  Dispatch  TeamMgr  Codegen
```

Fases 0–5: ✅ concluídas. Cobrem todo o escopo single-host do MCFlow.  
Fase 6: codegen — única fase pendente no roadmap.  
~~Fase de comunicação distribuída~~: removida do roadmap (fora de escopo).

---

### Fase 0 — Correção de Bugs Críticos

**Objetivo**: fazer o projeto compilar e estabelecer baseline testável.

**Arquivos modificados**: `deployment_plan.hpp`, `plans/deployment_plan.json`, `parser_json.cpp`

**Tarefas**:
- Adicionar `uint64_t period_ns` e `uint64_t deadline_ns` em `SubtaskInfo`
- Adicionar campos `period_ns` e `deadline_ns` em cada subtask no `plans/deployment_plan.json`
- Validar que parser usa `.value("period_ns", uint64_t(0))` com defaults seguros

**Critérios de Aceitação**:
- `make main` compila sem erros ou warnings
- `./main` executa e imprime `period_ns` e `deadline_ns` corretamente
- JSON sem os campos: executa com valores padrão 0, sem crash

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_parser_period_deadline` | Parsear JSON com campos; verificar `subtask.period_ns == valor_esperado` |
| `test_parser_defaults` | Parsear JSON sem campos; verificar que retorna 0 sem lançar exceção |
| `test_parser_all_fields` | Verificar que todos os campos de `SubtaskInfo` são populados corretamente |

---

### Fase 1 — DAG Completo

**Objetivo**: DAG com travessia topológica, detecção de ciclos e cálculo de profundidade — base para as fases seguintes.

**Arquivos modificados**: `dag.hpp`, `dag.cpp`

**Tarefas**:
- `topological_sort()` → `vector<int>` com IDs em ordem de execução (Kahn's algorithm)
- `has_cycle()` → `bool` via DFS com marcação de nós em processamento (cinza/branco/preto)
- `pipeline_depth()` → `int` com a profundidade máxima (longest path em DAG)
- `fan_in_count(int id)` e `fan_out_count(int id)` → contagem de predecessores/sucessores de um nó

**Critérios de Aceitação**:
- DAG linear A→B→C→D: `topological_sort()` retorna [A,B,C,D]; `pipeline_depth()` == 4
- DAG com ciclo A→B→A: `has_cycle()` == true
- DAG com fan-in (A,B)→C: `fan_in_count(C)` == 2
- DAG diamond (A→B, A→C, B→D, C→D): `pipeline_depth()` == 3; sort válido para D vir após B e C

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_dag_linear` | Chain de 4 nós; sort correto; depth == 4 |
| `test_dag_cycle` | Ciclo A→B→A; `has_cycle()` == true; `topological_sort()` lança exceção ou retorna vazio |
| `test_dag_fan_in` | Dois predecessores em C; `fan_in_count(C)` == 2 |
| `test_dag_diamond` | Diamond A→B,C→D; sort coloca D por último; depth == 3 |
| `test_dag_single_node` | DAG de um único nó; `pipeline_depth()` == 1; sort retorna [0] |

---

### Fase 2 — Ring Buffer Robusto

**Objetivo**: ring buffer fiel ao paper — cache-line padding, backpressure real e suporte a fan-in multi-supplier.

**Arquivos modificados**: `ring_buffer.hpp`  
**Arquivos novos**: nenhum (tudo em `ring_buffer.hpp` via templates)

**Tarefas**:
- Adicionar padding de cache-line via `alignas(64)` + struct wrapper por slot; `static_assert(sizeof(Slot) % 64 == 0)`
- Converter head/tail para `std::atomic<size_t>` (correto para produtor/consumidor em threads diferentes)
- Implementar `release(size_t seq_num)` funcional: incrementar contador atômico do consumidor; `write()` bloqueia se buffer cheio
- Criar `MultiSupplierRingBuffer<T, N, NumSuppliers>`: cada slot tem `NumSuppliers` campos `T data[NumSuppliers]` + bitfield `ready_mask`; slot liberado para consumo quando `ready_mask == (1 << NumSuppliers) - 1`

**Critérios de Aceitação**:
- `sizeof(Slot<double>) % 64 == 0` verificado em compile time
- Com buffer de capacidade 4: 5ª escrita bloqueia até que `release()` seja chamado
- `MultiSupplierRingBuffer<int,4,2>`: slot não consumível até ambos os suppliers escreverem
- Teste de produtor/consumidor concorrentes: 100.000 elementos sem dados corrompidos

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_ringbuf_cacheline` | `static_assert` ou assert que `sizeof(slot) % 64 == 0` |
| `test_ringbuf_backpressure` | Producer escreve N+1 com buffer cheio; verifica bloqueio até consumer liberar |
| `test_ringbuf_multifield` | 2 suppliers escrevem campo 0 e 1 separadamente; slot disponível apenas após ambos |
| `test_ringbuf_concurrent` | Producer/consumer em threads; 100k elementos; verificar integridade dos dados |
| `test_ringbuf_wraparound` | Verificar que slots se sobrescrevem corretamente após N escritas |

---

### Fase 3 — Modelo de Componente Completo

**Objetivo**: classificação source/intermediate/sink, adapter com lambda, mecanismo de pré-alocação.

**Arquivos modificados**: `component.hpp`, `adapter.hpp`

**Tarefas**:
- Adicionar `enum class ComponentKind { SOURCE, INTERMEDIATE, SINK }` e `virtual ComponentKind kind() const` em `ComponentBase`
- Criar `SourceComponent<O,C>` (sem `input_type`) e `SinkComponent<I,C>` (sem `output_type`)
- Mudar `AdapterFunction` de ponteiro de função para `std::function<downstream_input(const upstream_output&)>`
- Adicionar `virtual void preallocate()` em `ComponentBase` — chamado uma vez em init time antes do loop real-time

**Critérios de Aceitação**:
- `SourceComponent::kind()` retorna `SOURCE`; `SinkComponent::kind()` retorna `SINK`
- `Adapter` aceita lambda com capture: `[factor](const double& v){ return (int)(v * factor); }`
- `preallocate()` chamado exatamente uma vez antes do primeiro `execute()`
- Componentes existentes (`ComponentA`, `ComponentB` em `main.cc`) continuam compilando sem modificação

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_component_kinds` | Instanciar os 3 tipos; verificar `kind()` correto para cada |
| `test_adapter_lambda` | Adapter com lambda capturando variável; verificar conversão correta |
| `test_adapter_function_ptr` | Retrocompatibilidade: adapter com ponteiro de função ainda funciona |
| `test_preallocate_order` | Mock registra ordem de chamadas; `preallocate()` antes de `execute()` |

---

### Fase 4 — Dispatcher com Periodicidade, Fan-in e Release-Guard

**Objetivo**: Dispatcher fiel ao paper — timer queue, release-guard, `in-processing`, cópia automática de outputs, condição de fan-in.

**Arquivos modificados**: `dispatcher.hpp`, `dispatcher.cpp`  
**Arquivos novos**: `demultiplexer.hpp` (separação do Reactor)

**Tarefas**:
- Adicionar `period_ns_` e `next_release_ns_` por subtarefa registrada
- Criar `timer_queue_` (`priority_queue<TimerEntry, min-heap>` ordenada por `next_release_ns`)
- Implementar os 6 passos do release-guard no loop do Demultiplexer (Seção V-C do paper)
- Adicionar `std::atomic<bool> in_processing_` por `Subtask`
- Criar classe `Demultiplexer` separada que recebe eventos epoll e encaminha ao `Dispatcher` (padrão Reactor)
- Após `execute()` de uma subtarefa: copiar output para ring buffers downstream; verificar condição de fan-in; enfileirar downstream se satisfeita; enviar notificação
- Criar `IdleThread` por Dispatcher: `SCHED_FIFO` prioridade mínima; ativa subtarefa com menor expiration na timer queue quando CPU está ociosa

**Critérios de Aceitação**:
- Subtarefa com `period_ns=10ms`: 5 subjobs consecutivos; todos os intervalos medidos ≥ 10ms
- Dois notifies rápidos para subtarefa lenta: execução serial (segundo aguarda primeiro)
- Chain A→B: após A executar, B enfileirado automaticamente sem código manual
- Fan-in (A,B)→C: C só executa após A e B completarem para o mesmo job (mesmo sequence number)

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_dispatcher_period` | 5 subjobs; medir intervalos com `clock_gettime(CLOCK_MONOTONIC)`; todos ≥ `period_ns` |
| `test_dispatcher_in_processing` | Dois notifies para subtarefa de 50ms; verificar execução serial, não paralela |
| `test_dispatcher_release_guard` | Subjob liberado manualmente antes do período; verificar que espera o boundary |
| `test_dispatcher_chain` | Chain linear 3 cores; verificar output do nó 3 com input do nó 1 correto |
| `test_dispatcher_fanin` | Dois dispatchers (core 0 e 1) → core 2; verificar que core 2 executa apenas após ambos |
| `test_idle_thread` | Nenhum subjob pendente; CPU ociosa; idle thread avança subtarefa da timer queue |

---

### Fase 5 — Team Manager

**Objetivo**: ciclo de vida completo de um team com wiring automático a partir do DAG e protocolo de terminação.

**Arquivos modificados**: `team_manager.hpp`  
**Arquivos novos**: `team_manager.cpp`

**Tarefas**:
- Implementar classe `TeamManager`:
  - `initialize(const TaskInfo&, DAG&)`: usa topological sort do DAG; instancia componentes; cria dispatchers por core/prioridade; cria ring buffers dimensionados com `pipeline_depth()`; wires adapters
  - `start()`: transita para RUNNING; inicia todos os dispatchers
  - `stop()`: envia termination request em ordem topológica reversa; aguarda ACK de cada subtarefa; transita para TERMINATED; desaloca recursos
  - `on_subtask_exception(int subtask_id)`: inicia terminação de emergência transitando para TERMINATING
- Terminação executada na prioridade RT da tarefa (via `Dispatcher.notify()` com subtask especial)

**Critérios de Aceitação**:
- `initialize()` constrói toda a pipeline a partir de `TaskInfo` sem código manual de wiring
- Transições de estado corretas: CREATED→INITIALIZED→RUNNING→TERMINATING→TERMINATED
- `stop()` em RUNNING: todos os dispatchers param limpo; verificado com valgrind (zero leaks)
- Exceção em subtarefa intermediária: team termina sem deadlock em ≤ 2× o período da tarefa

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_team_lifecycle` | initialize → start → stop; verificar todos os estados |
| `test_team_auto_wiring` | Pipeline de 4 subtarefas; wiring feito por `initialize()`; output correto sem código manual |
| `test_team_exception` | Injetar exceção em subtarefa 2 de 4; verificar terminação limpa |
| `test_team_no_leak` | Rodar com valgrind após stop(); zero memory leaks |
| `test_team_double_stop` | Chamar `stop()` duas vezes; não crash (idempotente) |

---

### ~~Fase de Comunicação Distribuída~~ — Removida do Roadmap

> Esta fase foi descartada. A implementação cobre apenas um host multi-core.  
> `HostManager`, `NetworkConnector` e protocolo TCP **não serão implementados**.

---

### Fase 6 — Ferramenta de Deployment e Geração de Código

**Objetivo**: gerar C++ + Makefile automaticamente a partir do `plans/deployment_plan.json` para um único host multi-core.

**Arquivos novos**: `tools/codegen.cpp`

**Tarefas**:
- Ler `plans/deployment_plan.json`; emitir um `main_generated.cpp` com:
  - Instanciação de componentes por `component_type` (lido do plano)
  - `RingBuffer<T, N>` por aresta, com `N = TeamManager::ring_buffer_size(u, v)` calculado em tempo de geração
  - `static_assert(N >= pipeline_depth)` para cada buffer gerado
  - Wiring de adapters entre tipos adjacentes
  - Inicialização e start do `TeamManager`
- Emitir `Makefile` com target `run` que compila e executa o binário gerado

**Critérios de Aceitação**:
- `./codegen plans/deployment_plan.json` gera código que compila sem warnings
- Código gerado reproduz o comportamento de `main.cc` para a pipeline atual
- Ring buffer size calculado automaticamente; `static_assert` falha se `N < pipeline_depth`
- DAG diamond no plano: fan-in gerado corretamente

**Testes Direcionados**:

| Teste | O que verificar |
|---|---|
| `test_codegen_linear` | Plano linear de 4 subtarefas; código gerado compila e produz output correto |
| `test_codegen_diamond` | DAG diamond no JSON; fan-in e `fan_in_total=2` no código gerado |
| `test_codegen_buffer_size` | `static_assert` presente no código gerado; falha com N deliberadamente pequeno |

---

## 4. Estrutura de Arquivos

```
tests/
  test_parser.cpp          # Fase 0 ✅
  test_dag.cpp             # Fase 1 ✅
  test_ringbuf.cpp         # Fase 2 ✅
  test_component.cpp       # Fase 3 ✅
  test_dispatcher.cpp      # Fase 4 ✅
  test_team_manager.cpp    # Fase 5 ✅

examples/
  example_ring.cpp
  example_two_threads.cpp
  example_dispatcher.cpp
  example_pipeline.cpp     # dispatcher: cadeia periódica + fan-in
  example_team_manager.cpp # team manager: wiring automático via DAG

tools/
  codegen.cpp              # Fase 6 — pendente
```

Status do `make test`: **33 testes, 0 falhas** (Fases 0–5 concluídas).
