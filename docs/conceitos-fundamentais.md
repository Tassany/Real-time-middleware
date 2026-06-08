# Conceitos Fundamentais do MCFlow

---

## Supplier e Consumer

No MCFlow, **supplier** e **consumer** são os dois lados de uma aresta do DAG:

```
    B  ← supplier (quem produz o dado)
    │
    ▼
    D  ← consumer (quem consome o dado)
```

**Supplier** é a subtarefa que **produz** o output — ela escreve no ring buffer após executar.

**Consumer** é a subtarefa que **consome** aquele output — ela lê do ring buffer antes de executar.

Cada aresta do DAG corresponde a um ring buffer:

```
B ──[ring buffer B→D]──▶ D
C ──[ring buffer C→D]──▶ D
```

O mesmo nó pode ser supplier em uma aresta e consumer em outra — por exemplo B é consumer de A e supplier de D ao mesmo tempo.

---

## Fan-in e Fan-out no DAG

### Fan-out

Um nó com **múltiplos sucessores**. A saída de uma subtarefa alimenta várias subtarefas downstream ao mesmo tempo.

```
    A
   / \
  B   C
```

A tem fan-out de 2. Quando A termina, copia seu output para os ring buffers de B e de C e notifica os dois dispatchers.

Fan-out é simples: basta copiar o output de A para os dois ring buffers e enviar dois `notify()`.

### Fan-in

Um nó com **múltiplos predecessores**. A subtarefa só pode executar quando **todos** os suppliers já copiaram seus dados para o slot correspondente.

```
  B   C
   \ /
    D
```

D tem fan-in de 2. D só é despachado quando B **e** C concluíram para o mesmo job (mesmo sequence number).

Fan-in exige sincronização: o Dispatcher de D não pode executar D na primeira notificação que receber — seria com dado de apenas um dos suppliers.

### Diamond — o caso combinado

```
    A
   / \
  B   C
   \ /
    D
```

A tem fan-out de 2. D tem fan-in de 2. D só executa após B e C terminarem, que por sua vez só executam após A.

No deployment plan do projeto um diamond com 4 subtarefas ficaria assim:

| Aresta | Supplier | Consumer |
|--------|----------|----------|
| A → B  | A        | B        |
| A → C  | A        | C        |
| B → D  | B        | D        |
| C → D  | C        | D        |

---

## Cache-line Padding

O processador não busca bytes individuais da RAM — ele busca blocos de **64 bytes** chamados cache lines. O problema surge quando dois campos usados por threads diferentes cabem na mesma cache line:

```
[  slot 0 (thread A)  |  slot 1 (thread B)  ]  ← mesma cache line de 64 bytes
```

Quando a thread A escreve no slot 0, o hardware invalida a cache line inteira na thread B — mesmo que B nunca toque no slot 0. Isso se chama **false sharing** e degrada severamente o desempenho inter-core, exatamente onde o MCFlow deveria ter vantagem sobre o TAO.

A solução é garantir que cada slot ocupe exatamente uma cache line completa:

```cpp
struct Slot {
    T data;
    char padding[64 - sizeof(T)]; // preenche até 64 bytes
} __attribute__((aligned(64)));

static_assert(sizeof(Slot) % 64 == 0);
```

Agora cada slot vive em sua própria cache line — threads em cores diferentes não se interferem.

---

## Backpressure Real

No `RingBuffer` atual o `release()` é no-op — ele não faz nada. Isso significa que o produtor pode continuar escrevendo indefinidamente e **sobrescrever slots que o consumidor ainda não leu**:

```
Buffer de 4 slots:  [0][1][2][3]
Produtor escreve:    0  1  2  3  4 ← sobrescreve slot 0 que D não leu
```

Backpressure real significa que o produtor **bloqueia** quando o buffer está cheio, esperando o consumidor liberar espaço com `release()`:

```
Produtor tenta escrever o 5º elemento
→ buffer cheio (4/4)
→ produtor dorme
→ consumidor lê slot 0, chama release()
→ produtor acorda e escreve
```

Isso garante que nenhum dado é perdido ou corrompido mesmo quando um consumidor é mais lento que o produtor.

O tamanho correto do buffer é calculado automaticamente pelo deployment plan para minimizar bloqueios:

```
tamanho = max(2, ceil(deadline / period) + pipeline_depth)
```

---

## Fan-in Multi-Supplier no Ring Buffer

Para suportar fan-in, cada slot do ring buffer do consumer precisa ter **um campo por supplier** mais uma máscara de bits indicando quais já escreveram:

```
Slot do ring buffer de D (2 suppliers: B e C):
┌──────────────────────────────────────────────┐
│ data_from_B  │  data_from_C  │  ready_mask   │
│  (field 0)   │   (field 1)   │   = 0b11      │
└──────────────────────────────────────────────┘
```

O fluxo de execução fica:

```
B termina → escreve data_from_B → seta bit 0 → ready_mask = 0b01
C termina → escreve data_from_C → seta bit 1 → ready_mask = 0b11 → notifica D
D executa com os dados de B e C
```

D só é notificado quando `ready_mask == (1 << NumSuppliers) - 1`, ou seja, quando todos os bits estiverem setados. Enquanto só um supplier escreveu, D não é despachado — sem risco de executar com dado incompleto.

No código isso é implementado como `MultiSupplierRingBuffer<T, N, NumSuppliers>`, onde `NumSuppliers` é determinado pelo DAG em tempo de inicialização.
