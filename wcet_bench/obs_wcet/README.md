# Observed WCET na Raspberry Pi 5 — réplica do lado empírico de Li et al. (2024)

Reproduz a medição de *observed WCET* da Figura 10 de

> M. Li, K. Xiao, Y. Zhou, D. Huang. **WCET Analysis Based on Micro-Architecture
> Modeling for Embedded System Security.** *Applied Sciences* 14 (2024) 7277.

Só o lado empírico. A ferramenta de análise estática deles não está envolvida.

O protocolo, tal como descrito na Seção 5 do paper:

> *"Execution time in processor cycles is obtained by reading the generic timer
> register of the ARMv8-A CPU. The difference between the values before and after
> execution is the measured execution time of the program. Five measured
> execution times are averaged to obtain the final observed WCET."*

Benchmarks (subconjunto da Fig. 10, do Mälardalen WCET suite):
`insertsort`, `edn`, `fft1`, `adpcm`, `expint`, `cnt`, `matmult`, `fir`, `bs`.
O `fft1` é o que o paper chama de "fft".

## Como rodar

Exige **AArch64** — `CNTVCT_EL0` não existe em x86, e o build falha com uma
mensagem explícita se você tentar. Compile na própria Pi.

```sh
# copie o diretório de benchmarks inteiro; o Makefile lê ../<nome>.c
scp -r /home/tassany/Documents/Codes/wcet_bench pi@raspberrypi:~/

# na Pi:
cd ~/wcet_bench/obs_wcet
sudo ./run_all.sh              # N=20 (padrão)
sudo ./run_all.sh -n 5         # replica o N=5 exato do paper
```

Sem `sudo` também roda: perde SCHED_FIFO e o governor `performance`, fica mais
ruidoso, e o `env.txt` registra que a isolação foi parcial.

Opções: `-n RUNS`, `-w WARMUPS` (padrão 1), `-c CORE` (padrão 3), `-o OUTDIR`.
Para trocar o nível de otimização: `OPT=-O2 ./run_all.sh`.

## Saídas

| arquivo | conteúdo |
|---|---|
| `results/observed_wcet.csv` | uma linha por benchmark — o entregável |
| `results/raw_<bench>.csv` | cada execução individual (`benchmark,run,ticks,us,cycles`) |
| `results/env.txt` | kernel, modelo, GCC, governor, frequências, isolação, flags |

Colunas de `observed_wcet.csv`:

```
benchmark, n,
min_ticks, mean_ticks, median_ticks, max_ticks,
min_us,    mean_us,    median_us,    max_us,
min_cycles,mean_cycles,median_cycles,max_cycles,
cpu_freq_hz, timer_freq_hz
```

**`mean_*` é o "observed WCET" na definição do paper** (eles fazem média de 5
runs). **`max_*` é o maior tempo realmente observado** e é o proxy honesto de
pior caso. O relatório no terminal mostra os dois lado a lado com a razão
`max/mean`. Nenhum dos dois é um limite superior seguro: medição só reporta
caminhos que por acaso executaram.

## Três ressalvas sobre a fidelidade ao paper

**1. O generic timer não conta ciclos de CPU.** O paper diz "execution time in
processor cycles is obtained by reading the generic timer register", mas o
`CNTVCT_EL0` corre a `CNTFRQ_EL0` — 54 MHz na Pi, ~18,52 ns por tick —
independente do clock do núcleo. As colunas de ciclos aqui são **derivadas**,
`ticks × f_cpu / f_timer`, com `f_cpu` lido de
`/sys/.../cpufreq/scaling_cur_freq` antes e depois da bateria (se mudar no meio,
sai aviso). Ticks, µs e ciclos ficam em colunas separadas para a conversão ser
auditável em vez de embutida.

**2. Benchmarks curtos batem no piso do timer.** A 18,52 ns/tick, `bs` e
provavelmente `cnt` executam em poucos ticks e os dígitos finais são
quantização, não sinal. O harness avisa no stderr quando a mediana fica abaixo
de 50 ticks. Interprete essas linhas como ordem de grandeza. O overhead da
própria leitura do timer é medido e reportado no cabeçalho de cada benchmark.

**3. Tamanhos de entrada são os originais do Mälardalen.** O paper alterou o
`insertsort` para 1024 elementos ("we changed the length of the target reversed
array in insertsort to 1024"), e é por isso que ele domina as Figuras 9 e 10.
Aqui o `insertsort` roda com os 10 elementos originais, então **a magnitude dele
não vai bater com a Fig. 10** — foi uma escolha deliberada para manter o
benchmark canônico. O que deve se manter é o ordenamento relativo dos outros
oito. Para aproximar o paper, edite `../insertsort.c` (`unsigned int a[11]` e o
bloco de inicialização em `main`).

Além disso, a Pi 5 é Cortex-A76 a ~2,4 GHz e a Fig. 10 é uma Pi 4B com
Cortex-A75; magnitudes absolutas diferem por construção.

## Como funciona

Os `.c` do Mälardalen **não são modificados**. Um binário por benchmark:

- `-Dmain=bench_entry` renomeia o `main` do benchmark; o `main` de verdade vem
  do `harness.c`. Verificado: os 9 objetos exportam `bench_entry` e nenhum
  exporta `main`.
- `-DUPPSALAWCET` ativa o guard que o próprio `matmult.c` traz para suprimir
  `printf` e as chamadas de `ttime()`. Os demais já protegem seu I/O com
  `#ifdef DEBUG`.
- `-include bench_shim.h` neutraliza `printf`/`puts`/`putchar` como rede de
  segurança. Verificado com `nm`: nenhum dos 9 objetos referencia qualquer
  função de I/O, então a região cronometrada é computação pura.
- `-fno-builtin` impede o GCC de trocar `cos`/`fabs`/`memcpy` do benchmark pelas
  versões dele — `fft1.c` chega a definir o próprio `fabs` estático.
- `-O0` por padrão, seguindo a convenção Mälardalen/Chronos de manter o CFG
  fiel ao fonte.
- `isb` antes de cada leitura de `CNTVCT_EL0`, para o acesso ao contador não ser
  reordenado em relação ao código medido no pipeline out-of-order do A76.
- `mlockall(MCL_CURRENT|MCL_FUTURE)` no harness: page fault durante a medição
  apareceria como tempo de execução.
- 1 warm-up descartado antes das N execuções medidas.

Todos os benchmarks reinicializam seu estado a cada chamada (`adpcm` chama
`reset()`, `cnt` e `matmult` chamam `InitSeed()`/`Initialize()`, `insertsort`
reescreve `a[]`), então as N execuções são equivalentes. A única exceção é o
`ai[]` do `fft1`, que não é reinicializado — mas o fluxo de controle do `fft1`
depende só de `n` e `flag`, nunca dos dados, então a contagem de iterações é
idêntica em toda execução.
