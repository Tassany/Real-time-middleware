# Fase 4 — Dispatcher, Timer Queue, Fan-in e Idle Thread

## Contexto

A Fase 4 implementa o subsistema de *dispatching* do MCFlow conforme descrito nas Seções V-B e V-C do paper (Huang et al., 2012). Este subsistema é responsável por:

- Receber notificações de ativação de subtarefas (de upstream ou de timers).
- Garantir que o intervalo entre releases de subtarefas periódicas seja sempre ≥ ao período declarado (*release-guard protocol*).
- Sincronizar múltiplos suppliers antes de despachar uma subtarefa (fan-in).
- Propagar automaticamente a execução para subtarefas downstream.
- Liberar o core para subtarefas de mais alta prioridade enquanto o idle thread gerencia timers.

---

## Arquivos modificados / criados

| Arquivo | Papel |
|---|---|
| `dispatcher.hpp` | Implementação completa (Subtask, Dispatcher, TimerQueue) |
| `demultiplexer.hpp` | Wrapper do padrão Reactor; delega ao Dispatcher |
| `tests/test_dispatcher.cpp` | 6 testes de aceitação |

---

## Estrutura de dados

### `Subtask`

Representa uma unidade de trabalho com todos os metadados necessários para o escalonamento em tempo real.

```cpp
struct Subtask {
    std::string           name;
    std::function<void()> execute;   // trabalho a executar

    uint64_t period_ns       = 0;    // 0 = aperiódico
    uint64_t next_release_ns = 0;    // próximo release absoluto (CLOCK_MONOTONIC ns)

    std::atomic<bool> in_processing{false};  // guarda leader/followers

    int              fan_in_total    = 1;    // nº de suppliers esperados
    std::atomic<int> fan_in_received{0};     // contagem desta instância

    std::vector<SubtaskConn> downstream;     // sucessores a notificar após execução
};
```

**Decisões de projeto**

- `std::atomic` nos campos de sincronização: permite `notify()` ser chamado de qualquer thread sem travar o mutex principal.
- `std::function<void()>`: suporta lambdas com capture, essencial para o modelo de componente (Fase 3).
- `Subtask` é non-copyable: `std::atomic` não é copiável; referências devem ser por ponteiro.

### `SubtaskConn`

Liga um `Subtask` ao `Dispatcher` que o gerencia:

```cpp
struct SubtaskConn {
    Dispatcher*     dispatcher;
    struct Subtask* subtask;
};
```

Permite que downstream de A esteja em um Dispatcher diferente de A (execução em cores distintos).

### `TimerEntry` / `TimerQueue`

```cpp
struct TimerEntry {
    uint64_t release_ns;   // tempo absoluto de liberação
    Subtask* subtask;
};
using TimerQueue = std::priority_queue<TimerEntry,
                                       std::vector<TimerEntry>,
                                       std::greater<TimerEntry>>;  // min-heap
```

O min-heap garante que `dispatch_expired_timers()` processe sempre o timer mais próximo primeiro.

---

## O protocolo de 6 passos (release-guard)

Implementado em `Dispatcher::process_subtask()`. Corresponde diretamente à Seção V-C do paper.

```
Passo 1  — Thread retira uma subtarefa da fila FIFO.
Passo 2  — Verifica flag in_processing (guarda leader/followers).
           Se true → descarta (já executando); retorna.
Passo 3  — Verifica se next_release_ns ainda está no futuro.
Passo 4a — Se ainda no futuro → move para timer_queue; sinaliza idle thread.
Passo 4b — Se expirou → avança next_release_ns += period_ns; executa.
Passo 5  — Notifica automaticamente todos os downstream.
Passo 6  — Limpa in_processing.
```

```cpp
void process_subtask(Subtask* s) {
    // Passo 2
    if (s->in_processing.exchange(true)) return;

    uint64_t now = monotonic_ns();

    // Passos 3 & 4a
    if (s->period_ns > 0 && s->next_release_ns > 0 && now < s->next_release_ns) {
        s->in_processing.store(false);
        // defer to timer queue
        pthread_mutex_lock(&timer_mutex_);
        timer_queue_.push({s->next_release_ns, s});
        pthread_mutex_unlock(&timer_mutex_);
        uint64_t sig = 1;
        ::write(idle_efd_, &sig, sizeof(sig));
        return;
    }

    // Passo 4b: avança release
    if (s->period_ns > 0) {
        s->next_release_ns = (s->next_release_ns == 0)
            ? now + s->period_ns
            : s->next_release_ns + s->period_ns;
    }

    s->execute();                          // executa

    for (auto& conn : s->downstream)      // Passo 5
        conn.dispatcher->notify(conn.subtask);

    s->in_processing.store(false);        // Passo 6
}
```

**Por que `next_release_ns += period_ns` e não `now + period_ns`?**
O paper exige periodicidade estrita — o próximo release é calculado a partir do release anterior, não do tempo de execução. Isso compensa o jitter de execução e preserva o padrão de ativação absoluto.

---

## Fan-in

```cpp
void notify(Subtask* s) {
    int received = s->fan_in_received.fetch_add(1) + 1;
    if (received < s->fan_in_total) return;   // aguarda mais suppliers
    s->fan_in_received.store(0);              // reset para o próximo job

    pthread_mutex_lock(&queue_mutex_);
    queue_.push(s);
    pthread_mutex_unlock(&queue_mutex_);

    uint64_t sig = 1;
    ::write(efd_, &sig, sizeof(sig));
}
```

`fetch_add` é atômico: sem mutex, qualquer número de suppliers pode chamar `notify()` concorrentemente. A subtarefa é enfileirada exatamente quando o último supplier sinaliza. O reset para 0 garante que o próximo job começa com contagem limpa.

---

## Idle Thread

A idle thread roda na prioridade mínima do SCHED_FIFO (prio=1). Ela só ganha CPU quando o core está ocioso — exatamente a semântica desejada pelo paper para o gerenciamento de timers.

```
┌──────────────────────────────────────────────────────────────────┐
│  idle_loop()                                                     │
│                                                                  │
│  loop:                                                           │
│    epoll_wait(idle_epfd, timeout=10ms)                           │
│      → acorda por sinal de timer_queue ou por stop()            │
│    dispatch_expired_timers()                                     │
│      → move timers expirados para a fila FIFO do Dispatcher     │
│      → escreve efd_ para acordar a thread principal             │
└──────────────────────────────────────────────────────────────────┘
```

**Correção crítica de deadlock**: a idle thread usa `epoll_wait` com timeout, então acorda periodicamente mesmo sem sinal. Uma leitura bloqueante de `idle_efd_` quando o contador está em zero causaria deadlock. A solução é condicionar a leitura ao retorno do `epoll_wait`:

```cpp
int n = epoll_wait(idle_epfd, events, 1, 10);
if (n > 0) {
    uint64_t val;
    ::read(idle_efd_, &val, sizeof(val)); // drena um token apenas quando há sinal
}
dispatch_expired_timers(); // sempre verifica expirados
```

---

## Thread principal (Dispatcher loop)

```
┌────────────────────────────────────────────────────────────────────┐
│  loop():   core=N, prioridade=priority_, SCHED_FIFO               │
│                                                                    │
│  epoll_wait(efd_, timeout=20ms)                                    │
│    → dequeue subtask from FIFO                                     │
│    → process_subtask() [6 passos]                                  │
│    → drain loop: enquanto fila não vazia, continua processando     │
└────────────────────────────────────────────────────────────────────┘
```

O drain loop após `process_subtask()` evita retornar ao `epoll_wait` quando downstream já foram enfileirados (reduz latência intra-cadeia).

---

## Demultiplexer

Implementa o padrão Reactor do paper (Figura 2). Na configuração atual (thread única por dispatcher), serve como facade que encapsula a chamada ao protocolo de 6 passos:

```cpp
class Demultiplexer {
public:
    static void process(Subtask* s, Dispatcher& dispatcher) {
        dispatcher.process_subtask(s);
    }

    static bool all_suppliers_ready(const Subtask* s) {
        return s->fan_in_received.load(std::memory_order_acquire)
               >= s->fan_in_total;
    }
};
```

Em implementações futuras com o padrão leader/followers (múltiplas threads compartilhando um epoll), o `Demultiplexer` seria responsável por distribuir eventos entre as threads seguidoras.

---

## Testes de aceitação

| # | Nome | O que valida |
|---|---|---|
| T1 | `test_dispatcher_chain` | A→B→C via downstream: ordem de execução é 0,1,2 |
| T2 | `test_dispatcher_fanin` | fan_in_total=2: C não executa após só A; executa após A+B |
| T3 | `test_dispatcher_release_guard` | period=100ms, 4 jobs rápidos: intervalos ≥ 80ms (20ms jitter) |
| T4 | `test_dispatcher_in_processing` | notify durante execução lenta: sem execução concorrente |
| T5 | `test_dispatcher_aperiodic` | 3 notifies sem período: 3 execuções sequenciais |
| T6 | `test_demultiplexer_process` | `Demultiplexer::process()` delega corretamente |

---

## Diagrama de fluxo

```
Supplier A ─┐
            │  notify(C)           ┌─ timer_queue (idle thread)
Supplier B ─┤ ─────────────────►  │
            │  [fan_in: 2/2 ✓]    ├─ FIFO queue
Supplier C ─┘                     │      │
                                   │      ▼
                              Dispatcher::loop()
                                   │
                                   ▼
                            process_subtask(C)
                            [6-step release-guard]
                                   │
                                   ├─ in_processing=true
                                   ├─ check next_release_ns
                                   ├─ execute()
                                   ├─ notify downstream(D, E, ...)
                                   └─ in_processing=false
```

---

## Critérios de aceitação (conforme plano de implementação)

- [x] Subtarefa com `period_ns > 0`: dois subjobs nunca executam com intervalo < `period_ns` (tolerância de 20ms de jitter do idle thread).
- [x] `in_processing=true` durante execução: segunda notificação não causa execução concorrente.
- [x] Chain A→B: após A executar, B é automaticamente enfileirado sem código manual.
- [x] Fan-in (A,B)→C: C só executa após A e B completarem no mesmo job.
