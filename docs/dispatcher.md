# Dispatcher — arquitetura e funcionamento interno

**Arquivo-fonte:** `dispatcher.hpp`  
**Versão documentada:** não-preemptiva (antes da adição de `PreemptiveDispatcher`)

---

## 1. Posição no sistema

O MCFlow executa um grafo de subtasks periódicas em múltiplos núcleos com prioridades fixas. A
pilha tem três camadas:

```
deployment_plan.json
        │
        ▼
   TeamManager          ← lê o plano, agrupa subtasks, instancia Dispatchers
        │
        ▼
    Dispatcher          ← 1 thread SCHED_FIFO por par (core, priority)
        │
        ▼
     Subtask            ← unidade de trabalho: execute() + metadados RT
```

O `Dispatcher` é a camada de execução. Ele não sabe o que o subtask faz; apenas garante que o
`execute()` seja chamado no núcleo certo, na prioridade certa, no momento certo.

---

## 2. A struct `Subtask`

`Subtask` é a unidade de trabalho. É definida em `dispatcher.hpp` e transporta dois grupos de
informação: o que executar e quando/como executar.

```cpp
struct Subtask {
    int                   id;
    std::function<void()> execute;   // lógica da aplicação

    // --- metadados de escalonamento periódico ---
    uint64_t period_ns       = 0;    // período em ns; 0 = aperiódico
    uint64_t next_release_ns = 0;    // próximo instante de liberação (CLOCK_MONOTONIC)

    // --- proteção contra execução concorrente ---
    std::atomic<bool> in_processing{false};

    // --- fan-in (junção de múltiplos produtores) ---
    int              fan_in_total    = 1;
    std::atomic<int> fan_in_received{0};

    // --- grafo de dependências ---
    std::vector<SubtaskConn> downstream;
};
```

### `period_ns` e `next_release_ns`

- `period_ns = 0`: subtask aperiódico. Toda notificação dispara `execute()` imediatamente.
- `period_ns > 0`: subtask periódico. O Dispatcher aplica o **release guard**: se a notificação
  chegar antes de `next_release_ns`, a execução é adiada até esse instante.
- `next_release_ns = 0` no início: indica que a primeira ativação ainda não ocorreu. O release
  guard é ignorado na primeira vez; após executar, `next_release_ns` é calculado como
  `now + period_ns` e a partir daí é sempre incrementado por `period_ns` (modo estrito, sem
  drift).

### `in_processing`

Flag atômico que implementa o padrão **leader/followers**: se uma segunda notificação chegar
enquanto o subtask já está sendo executado, `process_subtask()` descarta a segunda chamada
imediatamente em vez de enfileirá-la. Isso evita execução concorrente de um mesmo subtask.

### `fan_in_total` e `fan_in_received`

Quantos produtores upstream devem notificar antes de o subtask ser executado. Configurado pelo
`TeamManager` a partir do grafo (`node.predecessors.size()`). O Dispatcher incrementa
`fan_in_received` a cada `notify()`; só enfileira quando `fan_in_received == fan_in_total`, e
em seguida zera o contador para o próximo ciclo.

### `downstream`

Lista de `SubtaskConn` apontando para os sucessores imediatos no grafo. O Dispatcher chama
`notify()` nesses conectores automaticamente após `execute()` terminar — nenhum código externo
precisa propagar o sinal manualmente.

```cpp
struct SubtaskConn {
    Dispatcher* dispatcher;  // dispatcher do subtask sucessor
    Subtask*    subtask;     // o próprio subtask sucessor
};
```

---

## 3. O modelo de dois threads

Cada `Dispatcher(core, priority)` cria **dois threads POSIX**, ambos afixados ao mesmo `core_`:

```
┌─────────────────────────────────────────────┐
│  Dispatcher(core=0, priority=50)            │
│                                             │
│  thread_ (SCHED_FIFO prio=50)              │
│    └── epoll_wait(efd_)                     │
│          └── process_subtask()              │
│                └── execute() + downstream  │
│                                             │
│  idle_thread_ (SCHED_FIFO prio=1)          │
│    └── epoll_wait(timerfd_ + idle_efd_)    │
│          └── dispatch_expired_timers()      │
└─────────────────────────────────────────────┘
```

**Thread principal (`thread_`):** roda com a prioridade real-time configurada (`priority_`). Fica
bloqueado em `epoll_wait(efd_)` esperando notificações de subtasks prontos para execução. Quando
acorda, desencadeia um subtask da fila FIFO interna e chama `process_subtask()`.

**Idle thread (`idle_thread_`):** roda sempre com `SCHED_FIFO prio=1` — a menor prioridade
real-time possível. Só executa quando o core está completamente ocioso (nenhuma thread RT de
prioridade maior está pronta). Monitora o `timerfd_` que é armado quando um subtask periódico
chega cedo demais. Quando o timer dispara, o idle thread re-enfileira o subtask e sinaliza o
`efd_` da thread principal.

A separação é necessária porque a thread principal não pode simplesmente chamar `sleep()` ao
encontrar um subtask que ainda não chegou à janela de liberação: ela serve potencialmente
múltiplos subtasks em fila. Bloquear a thread principal paralisaria todos os subtasks daquele
`(core, priority)`. O idle thread resolve isso sem bloquear a execução principal.

---

## 4. File descriptors internos

O `Dispatcher` usa quatro file descriptors do kernel para toda a comunicação assíncrona:

| FD | Tipo | Função |
|---|---|---|
| `efd_` | `eventfd(EFD_SEMAPHORE)` | Acorda a thread principal quando um subtask é enfileirado |
| `epfd_` | `epoll` | Multiplexador da thread principal (monitora apenas `efd_`) |
| `timerfd_` | `timerfd(CLOCK_MONOTONIC)` | Dispara no instante `next_release_ns` de subtasks periódicos adiados |
| `idle_efd_` | `eventfd(EFD_SEMAPHORE)` | Acorda o idle thread durante o `stop()` para desbloquear o `epoll_wait` |

`EFD_SEMAPHORE` faz com que cada `write(1)` adicione 1 ao contador do fd e cada `read()` consuma
exatamente 1. Isso garante que nenhuma notificação seja perdida mesmo que múltiplas chegues antes
de o thread acordar: o contador acumula tokens.

O idle thread tem seu próprio epoll local (criado dentro de `idle_loop()`, não compartilhado) que
monitora `timerfd_` e `idle_efd_` simultaneamente.

---

## 5. `notify()` — portão de fan-in

```cpp
void notify(Subtask* s) {
    int received = s->fan_in_received.fetch_add(1) + 1;
    if (received < s->fan_in_total) return;  // ainda aguardando produtores
    s->fan_in_received.store(0);             // reseta para o próximo ciclo

    pthread_mutex_lock(&queue_mutex_);
    queue_.push(s);
    pthread_mutex_unlock(&queue_mutex_);

    uint64_t sig = 1;
    ::write(efd_, &sig, sizeof(sig));        // acorda a thread principal
}
```

`notify()` é **thread-safe** e pode ser chamada de qualquer contexto — thread principal, outro
Dispatcher, ou código externo (como o `TeamManager::notify()` para disparar fontes). A operação
é wait-free no caminho comum: `fetch_add` é atômico e não há lock até o fan-in completar.

Quando `fan_in_total == 1` (caso mais comum), `notify()` sempre enfileira imediatamente.

---

## 6. `process_subtask()` — o protocolo de release guard (6 passos)

`process_subtask()` implementa o protocolo descrito na Seção V-C do paper. Os 6 passos originais
do paper são executados aqui:

```cpp
void process_subtask(Subtask* s) {
    // Passo 2: leader/followers — descarta se já está em execução
    if (s->in_processing.exchange(true)) return;

    uint64_t now = monotonic_ns();

    // Passos 3 e 4a: release guard periódico
    if (s->period_ns > 0 && s->next_release_ns > 0 && now < s->next_release_ns) {
        s->in_processing.store(false);

        pthread_mutex_lock(&timer_mutex_);
        timer_queue_.push({s->next_release_ns, s});
        uint64_t earliest = timer_queue_.top().release_ns;
        pthread_mutex_unlock(&timer_mutex_);

        struct itimerspec its{};
        its.it_value.tv_sec  = earliest / 1'000'000'000ULL;
        its.it_value.tv_nsec = earliest % 1'000'000'000ULL;
        timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
        return;  // ← sai sem executar; idle thread vai re-enfileirar no tempo certo
    }

    // Passo 4b: avança next_release_ns (periodicidade estrita)
    if (s->period_ns > 0) {
        s->next_release_ns = (s->next_release_ns == 0)
            ? now + s->period_ns          // primeira ativação
            : s->next_release_ns + s->period_ns;  // ativações seguintes
    }

    // Passo 4c: executa
    s->execute();

    // Passo 5: propaga para sucessores
    for (auto& conn : s->downstream)
        conn.dispatcher->notify(conn.subtask);

    // Passo 6: libera o flag de execução
    s->in_processing.store(false);
}
```

### Detalhe: periodicidade estrita vs. relativa

`next_release_ns + period_ns` (não `now + period_ns`) garante periodicidade estrita: o período
real-time não sofre drift mesmo se `execute()` demorar mais do que o esperado em algum ciclo.
Se houver atraso acumulado suficiente para a próxima ativação já ter passado quando a atual
termina, `now >= next_release_ns` na próxima vez e o release guard não bloqueia — o subtask
executa "em recuperação".

### A fila `timer_queue_`

É uma min-heap (`std::priority_queue` com `std::greater<TimerEntry>`), onde cada entrada é:

```cpp
struct TimerEntry {
    uint64_t release_ns;
    Subtask* subtask;
};
```

O `timerfd_settime` é armado sempre com o **menor** `release_ns` da heap (o próximo a disparar).
Quando o idle thread acorda com o timerfd disparado, chama `dispatch_expired_timers()` que drena
todos os entries com `release_ns <= now` de volta para a fila `queue_` da thread principal.

---

## 7. O caminho completo de uma ativação

Dois cenários:

### Caminho direto (notificação chega no tempo certo)

```
notify(s)
 ├── fan_in check  → ok
 ├── queue_.push(s)
 └── write(efd_, 1)
      │
      ▼
thread_ acorda de epoll_wait
 ├── read(efd_, &val)
 ├── queue_.pop() → s
 └── process_subtask(s)
      ├── in_processing = true
      ├── now >= next_release_ns → executa diretamente
      ├── next_release_ns += period_ns
      ├── s->execute()
      ├── for each downstream: dispatcher->notify(downstream_subtask)
      └── in_processing = false
```

### Caminho adiado (notificação chega antes do next_release_ns)

```
notify(s)
 └── write(efd_, 1)
      │
      ▼
thread_ acorda
 └── process_subtask(s)
      ├── now < next_release_ns
      ├── in_processing = false  (libera para futuras notificações)
      ├── timer_queue_.push({next_release_ns, s})
      └── timerfd_settime(next_release_ns)
               │
               │  (CPU livre; idle thread em epoll_wait com prio=1)
               ▼
idle_thread_ acorda quando timerfd dispara em next_release_ns
 └── dispatch_expired_timers()
      ├── timer_queue_.pop() → s  (release_ns <= now)
      ├── queue_.push(s)
      └── write(efd_, 1)
               │
               ▼
thread_ acorda (prio alta → preempta idle_thread_ imediatamente)
 └── process_subtask(s)
      ├── now >= next_release_ns → executa
      ├── s->execute()
      └── downstream propagation
```

No caminho adiado, o `in_processing` é **resetado para false** antes de retornar. Isso é
essencial: o subtask precisa estar disponível para receber novas notificações enquanto aguarda
na timer_queue_. Sem esse reset, uma segunda notificação seria descartada pelo leader/followers
guard.

---

## 8. `TeamManager` — quem cria e conecta os Dispatchers

O `Dispatcher` sozinho não sabe sobre o grafo, sobre quais subtasks existem ou sobre prioridades.
O `TeamManager` é responsável por:

### 8.1 Agrupamento por `(core, priority)`

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

Múltiplos subtasks com o mesmo `(core, priority)` compartilham **um único Dispatcher** — um
único thread, uma única `queue_`, um único `efd_`. Esta é a definição de escalonamento
particionado não-preemptivo: dentro de um nível de prioridade, os subtasks competem em fila FIFO.

### 8.2 Configuração de fan-in e downstream

```cpp
s->fan_in_total = static_cast<int>(node.predecessors.size());
if (s->fan_in_total < 1) s->fan_in_total = 1;

for (int succ_id : node.successors)
    s->downstream.push_back({subtask_dispatcher_.at(succ_id),
                              subtasks_.at(succ_id)});
```

O `fan_in_total` vem do número de predecessores no DAG. Nós sem predecessores (sources) ficam
com `fan_in_total = 1` — esperando uma única notificação do tick loop externo.

O downstream aponta para o Dispatcher **do sucessor** — que pode ser um Dispatcher diferente
(outro core ou outra prioridade). Isso é o que conecta os stages do pipeline.

### 8.3 Wrappers de exceção

O `TeamManager` envolve cada `execute()` original com um bloco `try/catch` que, em caso de
exceção, chama `on_subtask_exception()`. Isso previne que uma exceção não tratada mate
silenciosamente a thread do Dispatcher sem que o sistema saiba.

### 8.4 Ordem de start e stop

- **Start:** em ordem topológica (fontes primeiro). Garante que um subtask downstream não
  receba `notify()` antes de seu Dispatcher estar pronto.
- **Stop:** em ordem reversa (sinks primeiro). Garante que nenhum Dispatcher ativo envie
  `notify()` para um Dispatcher que já foi destruído.

---

## 9. Relação com `SubtaskInfo` e `DeploymentPlan`

A separação entre `SubtaskInfo` e `Subtask` reflete uma separação arquitetural:

| Estrutura | Origem | Contém |
|---|---|---|
| `SubtaskInfo` | `deployment_plan.json` via `JsonParser` | metadados estáticos: `id`, `core`, `priority`, `period_ns`, `deadline_ns`, `component_type` |
| `Subtask` | criado pelo código da aplicação | comportamento dinâmico: `execute()` lambda, estado RT (`next_release_ns`, `fan_in_received`) |

O `TeamManager::SubtaskEntry` combina os dois. O usuário cria os objetos `Subtask`, atribui o
`execute()`, e passa ambos ao `TeamManager::initialize()`. O TeamManager lê o `SubtaskInfo` para
decidir agrupamento e wiring, e configura o `Subtask` (period_ns, fan_in_total, downstream).

---

## 10. Relação com `DAG`

O `DAG` armazena a topologia pura: nós (`Node` com `id`, `predecessors`, `successors`) e arestas.
Ele não sabe sobre Dispatchers ou subtasks. O `TeamManager` usa:

- `dag.topological_sort()`: ordem de criação dos Dispatchers e configuração dos subtasks
  (garante que o downstream já existe quando o upstream é configurado).
- `dag.pipeline_depth()`: comprimento do caminho mais longo, usado na fórmula do ring buffer.
- `dag.nodes()`: iteração sobre todos os nós para configurar `fan_in_total` e `downstream`.

---

## 11. Ciclo de vida do Dispatcher

```
Dispatcher d(core, priority);   // construtor: só inicializa atributos, sem syscalls

d.register_subtask(s);          // adiciona à lista interna (subtasks_)

d.start();                      // cria eventfds, timerfd, epoll; lança thread_ e idle_thread_

// --- sistema rodando ---
d.notify(s);                    // pode ser chamado de qualquer thread

// --- shutdown ---
d.stop();
  ├── running_ = false
  ├── write(efd_, 1)        → desbloqueia thread_ do epoll_wait
  ├── write(idle_efd_, 1)   → desbloqueia idle_thread_ do epoll_wait
  ├── pthread_join(thread_)
  ├── pthread_join(idle_thread_)
  └── fecha todos os fds
```

O destrutor chama `stop()` implicitamente. O `TeamManager` gerencia os ciclos de vida via
`std::unique_ptr<Dispatcher>` — quando o TeamManager é destruído, todos os Dispatchers são
parados e desalocados automaticamente.

---

## 12. Propriedades do escalonamento resultante

**Preempção entre prioridades:** sim. O Linux SCHED_FIFO garante que, se a thread prio=80 fica
pronta enquanto prio=50 está executando no mesmo core, prio=80 preempta prio=50 imediatamente.
Essa preempção é automática — o Dispatcher não precisa fazer nada especial.

**Preempção dentro da mesma prioridade:** não. Se dois subtasks têm `(core=0, priority=50)`,
eles compartilham um único Dispatcher. O segundo só executa depois que o primeiro terminar. Isso
é o modelo **particionado não-preemptivo** do paper (Seção II).

**Fila FIFO dentro da mesma prioridade:** sim. A `std::queue<Subtask*>` é FIFO puro. Subtasks do
mesmo `(core, priority)` executam na ordem em que receberam seu `notify()` completo.

**Deadline enforcement:** não implementado diretamente no Dispatcher. O deadline existe em
`SubtaskInfo::deadline_ns` e é usado apenas para dimensionar o ring buffer
(`ring_buffer_size()`). A detecção de miss de deadline é responsabilidade do código de
aplicação dentro de `execute()`.
