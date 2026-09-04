# TeamManager — arquitetura e funcionamento interno

**Arquivos-fonte:** `team_manager.hpp`, `team_manager.cpp`  
**Versão documentada:** não-preemptiva (antes da adição de `PreemptiveTeamManager`)

---

## 1. Posição no sistema

O `TeamManager` é a camada de **orquestração**: ele lê os metadados de escalonamento do plano de
implantação, constroí os `Dispatcher`s, conecta os subtasks entre si seguindo o grafo de
dependências, e gerencia o ciclo de vida completo do sistema em execução.

```
plans/deployment_plan.json
        │  parser_json.cpp → DeploymentPlan
        ▼
   TeamManager
    ├── lê SubtaskInfo (core, priority, period_ns, deadline_ns)
    ├── agrupa por (core, priority) → instancia Dispatchers
    ├── lê DAG (predecessores, sucessores) → configura fan_in e downstream
    ├── calcula tamanho dos ring buffers → advisory para o gerador de código
    └── gerencia estado: CREATED → INITIALIZED → RUNNING → TERMINATING → TERMINATED
              │
              ▼  (1 Dispatcher por par único)
         Dispatcher
              │
              ▼
           Subtask
```

O `TeamManager` não executa nada ele mesmo. Ele constrói a infraestrutura e depois, durante a
execução, serve apenas como ponto de entrada para `notify()` — que repassa a chamada ao
`Dispatcher` correto.

---

## 2. Estruturas de dados internas

```cpp
class TeamManager {
    // (core, priority) → Dispatcher exclusivo desse par
    std::map<CorePrio, std::unique_ptr<Dispatcher>> dispatchers_;

    // Ordem de criação dos Dispatchers (seguindo ordem topológica do primeiro
    // subtask de cada par). Usada para start/stop na sequência correta.
    std::vector<CorePrio> dispatcher_order_;

    // subtask_id → ponteiro raw para o Dispatcher desse subtask
    std::map<int, Dispatcher*> subtask_dispatcher_;

    // subtask_id → ponteiro raw para o Subtask (não é dono; o chamador é dono)
    std::map<int, Subtask*> subtasks_;

    // (upstream_id, downstream_id) → N recomendado para RingBuffer<T, N>
    std::map<Edge, std::size_t> ring_buffer_sizes_;

    std::vector<int>  topo_order_;     // ordem topológica dos IDs de subtask
    mutable std::mutex state_mutex_;   // protege state_ e acesso a dispatchers_
    State              state_;
};
```

**Propriedade vs. referência:** o `TeamManager` é **dono** dos `Dispatcher`s (via
`unique_ptr`). Ele **não é dono** dos `Subtask`s — isso é responsabilidade do código da
aplicação. O `SubtaskEntry` combina o `SubtaskInfo` (metadados estáticos) com um ponteiro para
o `Subtask` já alocado pelo chamador.

---

## 3. A máquina de estados

```
CREATED ──initialize()──► INITIALIZED ──start()──► RUNNING
                                                        │
                                              stop() or │ on_subtask_exception()
                                                        ▼
                                                  TERMINATING ──do_stop()──► TERMINATED
```

Cada transição é protegida por `state_mutex_`. As restrições por estado:

| Estado | `initialize()` | `start()` | `notify()` | `stop()` |
|---|---|---|---|---|
| CREATED | ✓ | ✗ | ✗ | (noop) |
| INITIALIZED | ✗ | ✓ | ✗ | via do_stop() |
| RUNNING | ✗ | ✗ | ✓ | ✓ |
| TERMINATING | ✗ | ✗ | ✗ | (em progresso) |
| TERMINATED | ✗ | ✗ | ✗ | (noop) |

`stop()` é **idempotente**: pode ser chamado múltiplas vezes sem erro. `on_subtask_exception()`
nunca chama `stop()` diretamente — transiciona para TERMINATING e deixa o thread externo
(main) chamar `stop()` para evitar deadlock (um thread não pode dar `pthread_join` em si mesmo).

---

## 4. `SubtaskEntry` — a ponte entre plano e execução

```cpp
struct SubtaskEntry {
    SubtaskInfo info;     // metadados do JSON: id, core, priority, period_ns, deadline_ns
    Subtask*    subtask;  // objeto com execute() e estado RT (não possuído)
};
```

A separação existe porque `SubtaskInfo` vem do parsing do JSON (dados estáticos, conhecidos em
tempo de configuração) e `Subtask` é criado pelo código da aplicação (comportamento dinâmico,
conhecido em tempo de execução). O `TeamManager` usa `info` para decidir agrupamento e wiring, e
usa `subtask` para instalar os metadados RT (`period_ns`, `fan_in_total`, `downstream`) e
registrá-lo no `Dispatcher` correto.

---

## 5. `initialize()` — o algoritmo central

`initialize()` é onde toda a estrutura é montada. Executa em cinco fases sequenciais:

### Fase 1 — Construção dos mapas auxiliares

```cpp
std::map<int, const SubtaskInfo*> info_map;
for (const auto& e : entries) {
    subtasks_[e.info.id] = e.subtask;
    info_map[e.info.id]  = &e.info;
}
```

Dois mapas são construídos a partir dos `entries`: `subtasks_` (id → Subtask\*) e `info_map`
(id → SubtaskInfo\*). O `info_map` é local a `initialize()` — após retornar, os metadados
de scheduling já foram instalados nos `Subtask`s e o mapa não é mais necessário.

### Fase 2 — Ordenação topológica e validação

```cpp
topo_order_ = dag.topological_sort();

for (int id : topo_order_) {
    if (info_map.find(id) == info_map.end())
        throw std::runtime_error(...);
}
```

`dag.topological_sort()` executa o algoritmo de Kahn e retorna os IDs dos nós em ordem de
execução válida (predecessores antes de sucessores). Lança exceção se o grafo tem ciclo.

A validação garante que todo nó do DAG tem um `SubtaskEntry` correspondente. Isso detecta
erros de configuração onde o JSON de deployment não corresponde ao grafo usado.

A ordem topológica é usada em todas as fases seguintes como **a ordem canônica de iteração**:
garante que quando um Dispatcher downstream é configurado, seu upstream já foi criado.

### Fase 3 — Agrupamento e criação de Dispatchers

```cpp
for (int id : topo_order_) {
    const SubtaskInfo* info = info_map.at(id);
    CorePrio cp{info->core, info->priority};

    if (dispatchers_.find(cp) == dispatchers_.end()) {
        dispatchers_[cp] = std::make_unique<Dispatcher>(info->core, info->priority);
        dispatcher_order_.push_back(cp);
    }
    subtask_dispatcher_[id] = dispatchers_.at(cp).get();
}
```

Este é o coração do modelo particionado não-preemptivo. Para cada subtask (em ordem
topológica), o par `(core, priority)` é calculado. Se nenhum Dispatcher existe ainda para esse
par, um novo é criado. Se já existe, o subtask é associado ao Dispatcher existente.

O resultado: **exatamente um Dispatcher por par `(core, priority)` distinto**. Vários subtasks
com o mesmo par compartilham thread, fila e eventfd. A ordem em que cada par aparece pela
primeira vez em `dispatcher_order_` é preservada para garantir start/stop correto.

```
Exemplo com plans/deployment_plan.json (18 subtasks, 3 cores × 6 prioridades):

  subtask 1 (core=0, prio=80) → Dispatcher(0,80) criado, dispatcher_order_[0]
  subtask 2 (core=1, prio=80) → Dispatcher(1,80) criado, dispatcher_order_[1]
  subtask 3 (core=2, prio=80) → Dispatcher(2,80) criado, dispatcher_order_[2]
  subtask 4 (core=0, prio=65) → Dispatcher(0,65) criado ...
  ...
  
  Resultado: 18 Dispatchers para 18 subtasks (pois nenhum par se repete neste plano)
```

Se o plano tivesse dois subtasks em `(core=0, priority=50)`, apenas um Dispatcher seria criado
e os dois compartilhariam a mesma fila FIFO.

### Fase 4 — Configuração de cada subtask e wiring de conexões

```cpp
for (const auto& node : dag.nodes()) {
    int id = node.id;
    Subtask*           s    = subtasks_.at(id);
    const SubtaskInfo* info = info_map.at(id);

    // Instala metadados RT
    s->period_ns = info->period_ns;

    // Fan-in: quantos upstream precisam notificar antes de executar
    s->fan_in_total = static_cast<int>(node.predecessors.size());
    if (s->fan_in_total < 1) s->fan_in_total = 1;
    s->fan_in_received.store(0);

    // Downstream: onde propagar após execute()
    s->downstream.clear();
    for (int succ_id : node.successors)
        s->downstream.push_back({subtask_dispatcher_.at(succ_id),
                                  subtasks_.at(succ_id)});

    // Registro no Dispatcher (para suas bookkeeping internas)
    subtask_dispatcher_.at(id)->register_subtask(s);

    // Wrapping de exceção
    auto original_fn = s->execute;
    s->execute = [this, id, original_fn]() {
        try { original_fn(); }
        catch (...) { on_subtask_exception(id); }
    };
}
```

**`period_ns`:** copiado do `SubtaskInfo` para dentro do `Subtask`. O `Dispatcher` lê
`s->period_ns` para decidir se aplica o release guard.

**`fan_in_total`:** derivado de `node.predecessors.size()`. Nós sem predecessores (sources)
recebem `fan_in_total = 1` — precisam de uma notificação externa por ciclo. Nós com dois
predecessores recebem `fan_in_total = 2` — só executam quando ambos upstream notificam.

**`downstream`:** cada entrada aponta para um par `(Dispatcher*, Subtask*)`. Quando o
`process_subtask()` do Dispatcher termina de executar um subtask, ele chama
`conn.dispatcher->notify(conn.subtask)` para cada entrada — propagando o sinal para os
sucessores sem intervenção externa.

O **Dispatcher do sucessor** pode ser diferente do Dispatcher do atual (outro core, outra
prioridade). Isso permite pipelines que cruzam cores: subtask A no core 0 notifica subtask B
no core 1.

**Wrapping de exceção:** o `execute()` original é substituído por uma closure que envolve a
chamada original em `try/catch`. Exceções nunca chegam ao thread do Dispatcher — são capturadas
aqui, logadas, e disparam `on_subtask_exception()` que inicia o shutdown gracioso.

### Fase 5 — Dimensionamento dos ring buffers

```cpp
int depth = dag.pipeline_depth();

for (const auto& node : dag.nodes()) {
    for (int down_id : node.successors) {
        ring_buffer_sizes_[{up_id, down_id}] = ring_buffer_n(
            up_info->period_ns, down_info->deadline_ns, depth);
    }
}
```

O `TeamManager` calcula e armazena o tamanho recomendado de ring buffer para cada aresta do
DAG. Esse valor não é usado internamente — é advisory, consultado pelo gerador de código
(`tools/codegen`) e pelos testes para dimensionar `RingBuffer<T, N>` com o N correto.

---

## 6. A fórmula do ring buffer

```
N = next_pow2(max(2, ceil(deadline_downstream / period_upstream) + pipeline_depth))
```

Implementada em `ring_buffer.hpp`:

```cpp
inline constexpr std::size_t ring_buffer_n(
    std::uint64_t period_up_ns,
    std::uint64_t deadline_down_ns,
    int           pipeline_depth) noexcept
{
    std::size_t jif  = (deadline_down_ns + period_up_ns - 1) / period_up_ns;
    std::size_t base = jif + static_cast<std::size_t>(pipeline_depth);
    if (base < 2) base = 2;
    return ring_buffer_next_pow2(base);
}
```

**`ceil(D/T)`** — quantos jobs upstream podem ser liberados antes que o deadline downstream
expire. Em carga máxima, esses jobs estarão "em voo" simultaneamente nos slots do buffer.

**`pipeline_depth`** — comprimento do caminho mais longo no DAG (número de nós). Representa a
ocupação simultânea máxima de slots no pior caso de encadeamento de stages.

**`max(2, ...)`** — piso de double-buffering: mesmo com D/T = 1 e depth = 1, o produtor
precisa de pelo menos 2 slots para não bloquear enquanto o consumidor está processando.

**`next_pow2`** — o `RingBuffer` usa mascaramento de bits (`seq_num & (N-1)`) para indexação
sem divisão; N precisa ser potência de 2.

---

## 7. `start()` — ativando os Dispatchers

```cpp
void TeamManager::start() {
    std::lock_guard<std::mutex> lk(state_mutex_);
    for (const auto& cp : dispatcher_order_)
        dispatchers_.at(cp)->start();
    state_ = State::RUNNING;
}
```

Os Dispatchers são iniciados em **ordem de criação** (que segue a ordem topológica do primeiro
subtask de cada par). Isso garante que os Dispatchers de sources estejam prontos e escutando no
`epoll_wait` antes de qualquer `notify()` externo chegar.

`Dispatcher::start()` cria os file descriptors (`efd_`, `epfd_`, `timerfd_`, `idle_efd_`) e
lança as duas pthreads (`thread_` e `idle_thread_`). Depois de `start()` retornar, o sistema
está pronto para receber notificações.

---

## 8. `notify()` — ponto de entrada externo

```cpp
void TeamManager::notify(int subtask_id) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    if (state_ != State::RUNNING)
        throw std::runtime_error("not in RUNNING state");
    subtask_dispatcher_.at(subtask_id)->notify(subtasks_.at(subtask_id));
}
```

`notify()` é a única interface pública de ativação durante a execução. O tick loop externo
(em `evaluation.cpp`) chama `tm.notify(source_id)` a cada período para disparar as fontes.
O `TeamManager` repassa para o `Dispatcher` do subtask via `subtask_dispatcher_`, que executa o
protocolo de fan-in e, se completo, enfileira o subtask.

O lock em `notify()` **não é um gargalo** na prática: a operação é O(1) (lookup em map + 1
`notify()`) e dura microssegundos. O tick loop chama `notify()` poucas vezes por tick — nunca
de forma contínua.

Propagações **internas** (de um subtask para seu downstream após `execute()`) **não passam pelo
TeamManager**. Elas vão diretamente de `Dispatcher::process_subtask()` para o
`conn.dispatcher->notify()`. O `state_mutex_` não é adquirido nesse caminho.

---

## 9. `stop()` e `do_stop()` — desligamento ordenado

```cpp
void TeamManager::stop() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (state_ == State::TERMINATED || state_ == State::CREATED) return;
        if (state_ != State::TERMINATING) state_ = State::TERMINATING;
    }
    do_stop();  // fora do lock
}

void TeamManager::do_stop() {
    for (auto it = dispatcher_order_.rbegin(); it != dispatcher_order_.rend(); ++it)
        dispatchers_.at(*it)->stop();

    std::lock_guard<std::mutex> lk(state_mutex_);
    state_ = State::TERMINATED;
}
```

A transição para TERMINATING acontece **dentro** do lock, mas `do_stop()` é chamado **fora**
do lock. Isso é essencial: `Dispatcher::stop()` chama `pthread_join()` nos dois threads do
Dispatcher, e um dos threads pode estar em `on_subtask_exception()` tentando adquirir o mesmo
lock. Chamar `pthread_join` com o lock held causaria deadlock.

Os Dispatchers são parados em **ordem inversa** à de criação (sinks antes de sources). Isso
garante que nenhum Dispatcher ativo possa enviar `notify()` para um Dispatcher que já foi
parado e fechou seus file descriptors.

```
Ordem de criação (topológica):
  Dispatcher(0,80) → Dispatcher(1,80) → ... → Dispatcher(2,6)

Ordem de parada (reversa):
  Dispatcher(2,6) → ... → Dispatcher(1,80) → Dispatcher(0,80)
```

---

## 10. Relação com `DeploymentPlan` e `SubtaskInfo`

O `DeploymentPlan` é o produto do `JsonParser` sobre o arquivo JSON. Ele contém:

- `tasks`: lista de `TaskInfo`, cada um com lista de `SubtaskInfo`
- `connections`: lista de `ConnectionInfo` (upstream\_id → downstream\_id)
- `hosts`: informação de deploy (não usada pelo `TeamManager`)

O código da aplicação itera sobre `plan.tasks` e `plan.subtasks` para criar os objetos
`Subtask` com os lambdas de `execute()`, e então monta o vetor de `SubtaskEntry` passado para
`initialize()`. O `TeamManager` não acessa o `DeploymentPlan` diretamente — recebe apenas os
`SubtaskEntry`s.

```cpp
// Padrão de uso em evaluation.cpp:
std::vector<TeamManager::SubtaskEntry> entries;
for (auto& task : plan.tasks)
    for (auto& info : task.subtasks)
        entries.push_back({info, subtask_ptrs.at(info.id).get()});
TeamManager tm;
tm.initialize(entries, dag);
```

---

## 11. Relação com `DAG`

O `TeamManager` não constrói o `DAG` — o código da aplicação faz isso, lendo as `connections`
do `DeploymentPlan`:

```cpp
DAG dag;
for (auto& task : plan.tasks)
    for (auto& st : task.subtasks)
        dag.add_node(st.id, nullptr);
for (auto& conn : plan.connections)
    dag.add_edge(conn.upstream, conn.downstream);
```

O `TeamManager` usa o `DAG` apenas para consultas estruturais:

| Consulta | Onde usada |
|---|---|
| `dag.topological_sort()` | ordem de inicialização de Dispatchers e subtasks |
| `dag.nodes()` (com `.predecessors` e `.successors`) | derivação de `fan_in_total` e `downstream` |
| `dag.pipeline_depth()` | fórmula do ring buffer |

O `DAG` em si não sabe nada sobre Dispatchers, prioridades ou períodos — é topologia pura.

---

## 12. Relação com `RingBuffer`

O `TeamManager` **não usa** `RingBuffer` diretamente. O ring buffer é uma estrutura de dados de
comunicação entre produtores e consumidores; cabe ao código da aplicação (ou ao código gerado
por `tools/codegen`) declarar e usar `RingBuffer<T, N>` com o N adequado.

O papel do `TeamManager` é **calcular** esse N via `ring_buffer_size(upstream_id, downstream_id)`
e disponibilizá-lo para quem vai gerar o código ou instanciar os buffers:

```cpp
// Consulta advisory — usada pelo gerador de código
std::size_t n = tm.ring_buffer_size(1, 4);  // edge subtask 1 → subtask 4
// RingBuffer<MyData, n> buf;  // N precisa ser constexpr; usar no gerador
```

---

## 13. O padrão de dois passos na aplicação

Um detalhe importante de `evaluation.cpp` (e de qualquer uso correto do `TeamManager`):
os objetos `Subtask` são alocados **antes** de atribuir os lambdas de `execute()`.

```cpp
// Passo 1: aloca todos os Subtasks com lambda vazio
for (auto& st : task.subtasks)
    subtask_ptrs[st.id] = std::make_unique<Subtask>(st.id, []{});

// Passo 2: atribui lambdas reais (capturando ponteiro estável para o Subtask)
for (auto& info : task.subtasks) {
    Subtask* s = subtask_ptrs.at(info.id).get();
    s->execute = [s, &m, ...] {
        // pode capturar 's' com segurança porque o endereço não muda mais
        int64_t t_sched = s->next_release_ns - s->period_ns;
        ...
    };
}
```

Isso é necessário porque o lambda de `execute()` precisa capturar `s` como ponteiro para ler
`s->next_release_ns` em tempo de execução (que só é preenchido pelo Dispatcher após `start()`).
Se o `Subtask` fosse construído dentro do lambda ou movido após o Passo 2, o ponteiro capturado
ficaria inválido. A alocação com `unique_ptr` no Passo 1 fixa o endereço em memória.

---

## 14. Thread safety das operações públicas

| Método | Thread-safe? | Notas |
|---|---|---|
| `initialize()` | sim | protegido por `state_mutex_`; só pode ser chamado uma vez |
| `start()` | sim | protegido por `state_mutex_` |
| `stop()` | sim | idempotente; `do_stop()` fora do lock para evitar deadlock |
| `notify()` | sim | protegido por `state_mutex_`; O(1); pode ser chamado de qualquer thread |
| `on_subtask_exception()` | sim | chamado de threads do Dispatcher; protegido por lock |
| `state()` | sim | leitura protegida por lock |
| `dispatcher_count()` | sim | leitura protegida por lock |
| `ring_buffer_size()` | sim | leitura protegida por lock |

Propagações internas (Dispatcher A → Dispatcher B via `downstream`) **não passam pelo
TeamManager** e não adquirem `state_mutex_`. Elas são thread-safe porque `Dispatcher::notify()`
tem seu próprio `queue_mutex_` interno.

---

## 15. O que o TeamManager não faz

**Não verifica deadlines.** O `deadline_ns` de cada subtask é usado apenas para calcular o
tamanho do ring buffer. Detectar e reagir a deadline misses é responsabilidade do lambda
`execute()` da aplicação.

**Não detecta liveness.** Se um subtask trava em `execute()` e nunca retorna, o Dispatcher
fica aguardando indefinidamente. Não há watchdog ou timeout.

**Não gerencia memória compartilhada entre subtasks.** Passar dados entre subtasks (valores
produzidos por um stage e consumidos pelo próximo) é responsabilidade do código da aplicação,
tipicamente usando `RingBuffer` dimensionado com `ring_buffer_size()`.

**Não inicia o tick loop.** O tick loop externo (em `main()`) é responsável por chamar
`tm.notify(source_id)` a cada período. O `TeamManager` não tem relógio interno nem gera
notificações espontaneamente.
