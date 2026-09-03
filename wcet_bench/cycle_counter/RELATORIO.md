# Medição de WCET observado na Raspberry Pi 5

Relatório do que foi construído e do que foi medido em `wcet_bench/cycle_counter/`.

Estado: etapas 1 a 8 concluídas e validadas na placa.

## 1. Objetivo

Reproduzir o procedimento empírico de medição de "WCET observado" descrito em

> M. Li, K. Xiao, Y. Zhou, D. Huang. *WCET Analysis Based on Micro-Architecture
> Modeling for Embedded System Security.* Applied Sciences 14 (2024) 7277.

Seção 5, Results and Discussion, na íntegra e no original.

> "Execution time in processor cycles is obtained by reading the generic timer
> register of the ARMv8-A CPU. The difference between the values before and
> after execution is the measured execution time of the program. Five measured
> execution times are averaged to obtain the final observed WCET."

São três frases, e cada uma vira uma decisão de implementação. Ler o generic
timer, subtrair, promediar cinco execuções.

A implementação foi feita em etapas progressivas, cada uma um programa
independente e compilável, de modo que cada decisão de projeto pudesse ser
validada em separado antes da seguinte.

## 2. Plataforma

| item | valor |
|---|---|
| placa | Raspberry Pi 5, BCM2712, 4 x Cortex-A76 |
| arquitetura | AArch64 (ARMv8-A) |
| instrumento | `CNTVCT_EL0`, generic timer |
| `CNTFRQ_EL0` | 54,000 MHz, ou 18,52 ns por tick |
| clock do núcleo | 1500 MHz em repouso, 2400 MHz com governor `performance` |
| compilação dos benchmarks | `gcc -std=gnu89 -O0 -fno-builtin -fno-stack-protector` |
| compilação do harness | `g++ -std=c++17 -O0` |

Carga medida, seis programas do conjunto Mälardalen presentes em `wcet_bench/`,
usados sem qualquer modificação nas fontes. Toda adaptação foi feita por flags
de compilação.

`bsort100`, `crc`, `fft1`, `matmult`, `ud`, `statemate`.

## 3. O que foi construído

| arquivo | conteúdo |
|---|---|
| `etapa1.cpp` | esqueleto da medição com `clock_gettime(CLOCK_MONOTONIC)` |
| `etapa2.cpp` | leitura de `CNTVCT_EL0` e `CNTFRQ_EL0` via assembly inline (`mrs`) |
| `etapa3.cpp` | `isb`, clobber `"memory"`, proteção contra eliminação de código morto, medição do piso do instrumento |
| `etapa4.cpp` | protocolo do paper, com restauração de estado, aquecimento e estatísticas |
| `etapa5.cpp` | ligação com os seis benchmarks via `-Dmain=bench_entry` e `extern "C"` |
| `etapa6.cpp` | isolamento do SO com `sched_setaffinity`, `SCHED_FIFO`, `mlockall`, leitura do governor |
| `etapa7.cpp` | conversão para microssegundos e ciclos, saída em CSV |
| `Makefile` | um alvo por etapa, um binário por benchmark, alvo `medir` |

Os binários ficam em `bin5/`, `bin6/` e `bin7/`, um por benchmark, para que as
etapas possam ser comparadas entre si.

## 4. Resultado final

Condições, `n=100`, 5 aquecimentos, núcleo 3, `SCHED_FIFO` prioridade 80,
`mlockall`, governor `performance`, clock estável em 2400 MHz. Ciclos contados
por `perf_event_open(PERF_COUNT_HW_CPU_CYCLES)` com `exclude_kernel=1`, e
descontado o piso do instrumento, que ficou entre 178 e 186 ciclos.

**Esta é a tabela a reportar.**

| benchmark | ciclos (média) | ciclos (máx) | tempo (média) | tempo (máx) |
|---|---|---|---|---|
| matmult | 115.325 | 115.572 | 48,05 µs | 48,16 µs |
| bsort100 | 72.754 | 72.866 | 30,31 µs | 30,36 µs |
| crc | 1.970 | 2.014 | 0,821 µs | 0,839 µs |
| ud | 1.894 | 2.012 | 0,789 µs | 0,838 µs |
| fft1 | 1.790 | 2.000 | 0,746 µs | 0,833 µs |
| statemate | 496 | 560 | 0,207 µs | 0,233 µs |

A média corresponde à definição de "observed WCET" de Li et al. e é a coluna
comparável com o paper. O máximo é o proxy honesto de pior caso.

Qualidade de cada linha, e comparação com o generic timer da etapa 7.

| benchmark | resolução pela PMU | resolução pelo timer | ganho | piso descontado |
|---|---|---|---|---|
| matmult | 0,0009 % | 0,04 % | 44x | 186 ciclos |
| bsort100 | 0,0014 % | 0,06 % | 45x | 183 ciclos |
| crc | 0,047 % | 2,17 % | 47x | 178 ciclos |
| ud | 0,048 % | 2,27 % | 47x | 180 ciclos |
| fft1 | 0,051 % | 2,41 % | 47x | 178 ciclos |
| statemate | 0,148 % | 8,33 % | 56x | 181 ciclos |

Com a PMU os seis benchmarks passam a ter as quatro estatísticas utilizáveis,
inclusive o máximo. Com o generic timer apenas dois tinham.

### O que fica de fora destes números

O `exclude_kernel` faz a contagem parar enquanto a CPU está em EL1. Os valores
acima são portanto o custo do **programa**, sem interrupções, tratadores de
temporizador ou preempção. Essa é a grandeza correta para comparar com uma
análise estática, que modela o programa e não o sistema operacional, mas não é o
tempo de parede que a tarefa consome numa aplicação real. A seção 5.3 quantifica
a diferença.

### Incerteza

Três fontes, em ordem de importância.

A subtração do piso é a dominante. Ela supõe que o custo do instrumento se soma
linearmente ao do benchmark, o que num núcleo fora de ordem é aproximação. Para
o `matmult` o piso é 0,16 % do valor bruto e a aproximação é irrelevante. Para o
`statemate` é 27 %, e por isso essa linha merece dois algarismos significativos,
`0,21 µs`, ou uma incerteza declarada da ordem de 4 %.

A resolução, de um ciclo, contribui entre 0,001 % e 0,15 %.

A conversão de ciclos para tempo supõe 2400 MHz exatos, lidos de
`scaling_cur_freq` antes e depois do lote e confirmados idênticos.

## 5. Achados

### 5.1 O registrador do paper não conta ciclos de processador

O `CNTVCT_EL0` é o generic timer. Ele tica a 54 MHz fixos, gravados pelo
firmware no boot, independentemente do clock do núcleo. Ciclos de processador
não são medidos, são derivados por `ticks x f_núcleo / f_timer`, e a fórmula
supõe que o clock ficou parado durante o lote.

A validação empírica foi feita medindo `matmult` em três clocks distintos.

| clock do núcleo | mediana (ticks) | ciclos derivados |
|---|---|---|
| 1500 MHz | 4156 | 115.444 |
| 1800 MHz | 3459 | 115.300 |
| 2400 MHz | 2595 | 115.333 |

A contagem de ticks varia 38 % entre o primeiro e o terceiro caso. Os ciclos
concordam dentro de 0,125 %. Isso confirma três coisas de uma vez. O
instrumento mede tempo e não trabalho. A conversão é válida sob clock estável.
E o `matmult` é limitado por processamento, já que um programa limitado por
memória não escalaria com o clock do núcleo e os três valores divergiriam.

Por isso o programa lê `scaling_cur_freq` antes e depois do lote, e quando a
frequência muda a coluna de ciclos sai vazia no CSV em vez de conter um número
sem significado.

### 5.2 A régua não resolve quatro dos seis benchmarks

Um tick de 18,52 ns equivale a 44 ciclos de núcleo a 2400 MHz. O `statemate`
inteiro custa cerca de 533 ciclos. Medir 533 ciclos com uma régua cuja menor
divisão vale 44 é o que produz os 8,33 % de resolução relativa da tabela.

A causa não é a placa. O conjunto Mälardalen foi montado para processadores
embarcados de dezenas de MHz, onde esses programas levavam milissegundos. Num
Cortex-A76 a 2400 MHz eles levam de 0,2 a 1 microssegundo. O instrumento que o
paper especifica não acompanhou o avanço do hardware.

A dispersão observada nesses quatro casos é compatível com quantização pura. O
`crc`, por exemplo, tem amostras entre 44 e 47 ticks, ou seja uma faixa de 3
ticks, e 1 tick vale 2,17 % da medida.

Compatível não quer dizer causada por. A etapa 8 mediu os mesmos programas com
resolução de um ciclo e mostrou que **parte da dispersão é real**: `fft1` 11,6 %,
`statemate` 10,4 %, `ud` 6,9 %, `crc` 3,7 %, tudo já com o kernel excluído. É
variação microarquitetural entre execuções. O generic timer não conseguia
distinguir as duas causas, e o `statemate` ilustra bem o problema: as cem
execuções produziram apenas três valores distintos, 12, 13 e 14 ticks, não
porque o programa tenha três comportamentos mas porque a régua não sabe escrever
nada entre eles. Pela PMU as mesmas cem execuções se espalham entre 671 e 741
ciclos.

### 5.3 A média de cinco execuções não estima o pior caso

Comparação entre o protocolo do paper e um lote de 100 execuções, nas mesmas
condições de isolamento.

| benchmark | média n=5 | média n=100 | desvio | máximo n=5 | máximo n=100 | desvio |
|---|---|---|---|---|---|---|
| matmult | 2595,4 | 2600,6 | +0,2 % | 2598 | 3146 | +21,1 % |
| bsort100 | 1637,2 | 1644,4 | +0,4 % | 1654 | 2362 | +42,8 % |

A média é um estimador estável. Ela praticamente não se move ao passar de 5
para 100 amostras. O problema é que ela estima a quantidade errada.

O máximo medido pelo generic timer cresce 21 % no `matmult` e 43 % no
`bsort100` só por olhar mais amostras. Em ciclos derivados, o `matmult` sai de
um "WCET observado" de 115.584 para um máximo de 139.822.

**Essas excursões não são o programa.** A etapa 8 permitiu separar as duas
coisas, e o resultado desfaz a leitura anterior.

| benchmark | dispersão pelos ticks | dispersão pela PMU, kernel excluído |
|---|---|---|
| matmult | 15,6 % | 0,4 % |
| bsort100 | 16,8 % | 0,4 % |

Com o `exclude_kernel` ligado, os ciclos do `matmult` ficam entre 115.303 e
115.758, uma faixa de 0,4 %. Aquela amostra de 2995 ticks, 15 % acima da mediana,
era tempo gasto em interrupção do sistema operacional, e não variação do
programa. O máximo sobre média do programa é de apenas 1,002.

Segue que promediar cinco execuções **não** subestima materialmente o pior caso
do programa nos dois benchmarks longos, ao contrário do que uma versão anterior
deste relatório afirmava. O que a média esconde é a interferência do sistema
operacional, que é outra coisa.

Nos quatro benchmarks curtos há variação real, agora visível porque a PMU a
resolve: `fft1` 11,6 %, `statemate` 10,4 %, `ud` 6,9 %, `crc` 3,7 %, tudo isso
já com o kernel excluído. É acomodação microarquitetural entre execuções, cache
e preditor de saltos.

A conclusão metodológica que fica é outra, e mais forte. O instrumento que o
paper especifica **não consegue separar o programa do sistema operacional**. O
máximo que o generic timer entrega é dominado por interferência do SO, e
compará-lo com uma análise estática que modela apenas o programa seria injusto
com a análise. Fazer essa separação exige a PMU com `exclude_kernel`.

Eliminar as interrupções em vez de apenas descontá-las exigiria isolamento no
boot, `isolcpus` e `nohz_full`, que não foi feito.

### 5.4 O `crc` tem estado estático que falseia o lote

A função `icrc()` do `crc.c` declara `static unsigned short icrctb[256], init=0`
e constrói a tabela de 256 entradas apenas na primeira chamada. Medido sem
aquecimento, o resultado é este.

| execução | ticks |
|---|---|
| 1 | 1459 |
| 2 a 5 | cerca de 73 |

A primeira execução custa 20 vezes as demais. Com um aquecimento, a construção
da tabela cai inteira no descarte e nenhuma amostra medida a inclui, o que
produz média de 72 ticks. Sem aquecimento, a média das cinco dá 350 ticks, que
não é nem o custo da primeira execução nem o do regime permanente.

O "WCET observado" do `crc` vale portanto 350 ou 72 dependendo do número de
aquecimentos, um fator de cinco decidido por uma escolha metodológica que o
paper não menciona.

O conserto correto seria uma medição por processo, relançando o binário a cada
amostra. Não foi implementado. O programa em vez disso torna o problema visível
através da opção `-w`.

Problema análogo e menor no `fft1`, cujo `main()` reescreve `ar[]` a cada
chamada mas não `ai[]`, de modo que a partir da segunda execução o benchmark
opera sobre dados diferentes.

### 5.5 Um aquecimento não basta

Com um único aquecimento, `ud` e `fft1` mostravam queda monotônica ao longo do
lote, de 60 para 45 ticks no caso do `ud`, com o clock já travado em 2400 MHz.
Não era escalonamento de frequência, era acomodação microarquitetural, cache de
instruções e preditor de saltos.

Com cinco aquecimentos a queda desaparece e as medianas baixam.

| benchmark | mediana com w=1 | mediana com w=5 | dispersão com w=1 | dispersão com w=5 |
|---|---|---|---|---|
| ud | 50 | 44 | 30,0 % | 9,1 % |
| fft1 | 47 | 42 | 21,3 % | 7,1 % |

### 5.6 O `bsort100` não roda em Linux sem intervenção

A fonte abre com `#define KNOWN_VALUE (int)(*((char *)0x80200001))` e usa a
macro em `Initialize()`. O benchmark lê um endereço absoluto esperando encontrar
o valor 1, o que funcionava no ambiente sem sistema operacional para o qual foi
escrito. Num processo Linux o endereço não está mapeado e a execução termina em
SIGSEGV.

A solução adotada mantém a fonte intocada. O harness chama
`mmap(0x80200000, 4096, MAP_FIXED_NOREPLACE)` e escreve 1 no byte de offset 1
antes de invocar o benchmark.

### 5.7 O escalonamento de frequência domina a variação entre rodadas

Antes do isolamento, o `matmult` mediu 4149 ticks numa rodada e 2596 noutra,
razão de 1,598, que é exatamente 2400 MHz dividido por 1500 MHz. Cada lote era
internamente consistente, com dispersão abaixo de 0,2 %, e os lotes discordavam
entre si em 60 %.

A causa é que um lote de cinco execuções de dezenas de microssegundos é curto
demais para o governor `ondemand` reagir. O lote inteiro roda no clock em que a
máquina já estava, que depende do que foi executado nos segundos anteriores. Na
prática, compilar antes de medir deixava a máquina a 2400 MHz e medir com a
máquina ociosa deixava a 1500 MHz.

Com `performance` a reprodutibilidade entre rodadas passou a ser exata, com
mediana de 2595 ticks em execuções repetidas.

### 5.8 O nível de otimização vale um fator 8

Um mesmo insertion sort de 1024 elementos, mesmo instrumento e mesma máquina,
mediu 3,5 ms compilado com `-O0` e 0,447 ms com `-O2`.

Segue que "WCET observado" não é propriedade do algoritmo, é propriedade do par
formado pelo algoritmo e pela linha de compilação. Qualquer número reportado
precisa vir acompanhado das flags, sem o que não é reproduzível nem comparável
com estimativa estática alguma.

Relacionado, a etapa 3 documenta que o compilador pode eliminar por completo o
código medido quando o resultado não é observável, e que a capacidade de fazer
isso depende do formato do código. Um laço com recorrência escalar simples é
removido em `-O2`. Um insertion sort aninhado com laço interno dependente dos
dados não é. Isso justifica a convenção da área de compilar benchmarks de WCET
com `-O0`.

## 6. Confronto com Li et al. (2024)

### 6.1 Hardware

Os autores usaram duas placas, uma Raspberry Pi 4 Model B e uma Firefly
ROC-RK3568-PC-SE. Nenhuma das duas é a Pi 5 usada aqui.

As legendas das Figuras 10 e 11 dizem "Crotex-A75" e "Crotex-A55". Além da
grafia, a atribuição da primeira está errada. A Raspberry Pi 4 Model B usa um
BCM2711 com quatro Cortex-A72 a 1,5 GHz, e não um A75. A Pi 5 usada aqui é um
BCM2712 com Cortex-A76 a 2,4 GHz. São três microarquiteturas distintas, e a
comparação numérica direta entre este trabalho e o paper não é legítima sem
essa ressalva.

### 6.2 Conjunto de benchmarks

O conjunto do paper, extraído das Figuras 10 e 11, é

`insertsort`, `edn`, `fft`, `adpcm`, `expint`, `cnt`, `matmult`, `fir`, `bs`.

O conjunto medido aqui é

`bsort100`, `crc`, `fft1`, `matmult`, `ud`, `statemate`.

A interseção é de apenas dois programas, `matmult` e `fft`. Todos os nove do
paper existem em `wcet_bench/`, de modo que alinhar os conjuntos é possível e
custa apenas mudar a variável `BENCHES` do Makefile. Com uma ressalva, a de que
os autores declaram ter alterado o `insertsort` para ordenar um vetor invertido
de 1024 elementos, e não dizem se alteraram os demais.

### 6.3 Dados da Figura 10, e uma inconsistência

Valores lidos da figura, para a Raspberry Pi 4. A coluna "Measured WCET" do
paper é a estimativa estática, e não uma medição, apesar do nome.

| benchmark | estimado | observado | razão impressa | razão calculada |
|---|---|---|---|---|
| insertsort | 1.983.695 | 1.568.494 | 1,26 | 1,265 |
| edn | 468.729 | 125.861 | 3,72 | 3,724 |
| fft | 181.048 | 20.065 | 1,51 | **9,02** |
| adpcm | 76.156 | 46.349 | 1,64 | 1,643 |
| expint | 59.958 | 42.831 | 1,40 | 1,400 |
| cnt | 48.748 | 35.617 | 1,37 | 1,369 |
| matmult | 19.831 | 16.816 | 1,18 | 1,179 |
| fir | 14.059 | 9.557 | 1,47 | 1,471 |
| bs | 9.706 | 8.102 | 1,20 | 1,198 |

Oito das nove razões conferem até a terceira casa. A do `fft` não. Os dois
valores impressos dão 9,02, e a figura mostra 1,51.

A Figura 11, para a outra placa, é internamente consistente nas nove linhas,
inclusive no `fft`, cujos 205.736 sobre 160.745 dão os 1,28 impressos.

Há duas leituras possíveis. Ou o valor observado de 20.065 está correto e o
rótulo de razão está errado, ou a razão está correta e o valor observado
deveria ser cerca de 119.900. A medição feita aqui favorece a primeira, já que o
`fft1` mediu 1.867 ciclos na Pi 5 com `-O0`, e um fator de otimização somado à
diferença de microarquitetura aproxima esse valor de 20.065 muito mais do que de
119.900.

Se a primeira leitura estiver certa, a afirmação de que "some estimated WCET
values for benchmarks are up to 2–3 times larger than the observed WCET" não
cobre o caso, porque 9,02 fica bem fora dessa faixa.

### 6.4 O `matmult`, único ponto de comparação limpo

| fonte | plataforma | compilação | ciclos observados |
|---|---|---|---|
| Li et al., Figura 10 | Pi 4, Cortex-A72, 1,5 GHz | não declarada | 16.816 |
| este trabalho | Pi 5, Cortex-A76, 2,4 GHz | `-O0` | 115.584 |

Fator de 6,87 entre os dois, e no sentido contrário ao esperado. O Cortex-A76 é
mais eficiente por ciclo que o A72, de modo que o mesmo programa deveria custar
menos ciclos e não sete vezes mais. O tamanho da matriz não explica, já que
`UPPERLIMIT` vale 20 na fonte original e não foi alterado aqui.

A explicação mais provável é o nível de otimização, que o paper não declara. A
seção 5.8 deste relatório mediu um fator de 8 entre `-O0` e `-O2` para uma carga
comparável. Aplicando esse fator, 115.584 dividido por 8 dá 14.448, que fica ao
lado dos 16.816 do paper.

Isso é verificável com um comando, recompilando o `matmult` com `-O2`, e é o
próximo experimento recomendado. Se confirmar, significa que os autores mediram
código otimizado enquanto o analisador estático deles trabalhava sobre um
modelo de pipeline em ordem, e que reproduzir o trabalho exige descobrir o nível
de otimização por inferência, já que ele não está no texto.

### 6.5 O que é crítica ao paper e o que não é

Em favor dos autores, a resolução do generic timer era adequada para o conjunto
que eles mediram. O menor valor da Figura 10 é o `bs` com 8.102 ciclos, o que a
1,5 GHz equivale a 292 ticks de 27,8 ciclos cada, ou 0,34 % de resolução. O
problema de resolução documentado na seção 5.2 deste relatório decorre de duas
escolhas nossas, a de medir programas menores que os deles e a de usar uma placa
com clock mais alto, que piora a razão entre ciclos e ticks.

Permanecem como lacunas reais do paper as seguintes.

A grandeza reportada. O eixo vertical das Figuras 10 e 11 diz "Processor Cycle"
e o instrumento declarado é o generic timer, que não conta ciclos. Houve
portanto uma conversão, e o texto não informa nem a frequência usada nem se o
clock foi fixado durante as medições. Numa Pi 4 com o governor padrão a
frequência varia entre 600 MHz e 1,5 GHz, uma faixa de 2,5 vezes, maior ainda que
a faixa de 1,6 medida aqui na Pi 5.

A impossibilidade de separar programa e sistema operacional. Esta substitui uma
crítica anterior deste relatório, que os dados da etapa 8 refutaram e que fica
registrada aqui para não se perder.

A versão anterior sustentava que promediar cinco execuções subestima o pior caso
o suficiente para derrubar as razões da Figura 10 abaixo de 1. O raciocínio era
que o máximo do `matmult` fica 21 % acima da média, e que as razões têm o
observado no denominador, de modo que a razão de 1,18 cairia para cerca de 0,98.

A etapa 8 mostrou que aqueles 21 % são interferência do sistema operacional e
não variação do programa. Medido com a PMU e `exclude_kernel`, o máximo sobre
média do `matmult` é 1,002. Recalculando com esse número, a razão de 1,18 vira
1,177, ou seja praticamente nada muda. **A crítica não se sustenta.**

O que sobrevive é a observação de que o generic timer não distingue as duas
coisas. Como a ferramenta de Li et al. modela pipeline, cache e preditor mas não
modela o sistema operacional, o comparando correto para a estimativa dela é o
custo do programa isolado, que só a PMU com `exclude_kernel` fornece. Usar a
média do timer, como os autores fizeram, aproxima esse valor melhor do que usar
o máximo do timer, ainda que por acidente e sem que o texto discuta a questão.

O nível de otimização, ausente do texto, conforme a seção 6.4.

## 7. Limitações conhecidas

1. Resolvida na etapa 8. Com o `CNTVCT_EL0` quatro dos seis benchmarks não
   tinham resolução suficiente; com a PMU os seis passam a ter.
2. Não há controle de estado de cache entre execuções. As amostras medem regime
   permanente com cache quente, o que não é comparável com estimativa estática,
   já que analisadores estáticos supõem cache vazia na entrada do programa.
3. O `crc` é medido em regime permanente, sem a construção da tabela.
4. As interrupções do sistema operacional não foram eliminadas, apenas
   reduzidas. Falta isolamento no arranque com `isolcpus` e `nohz_full`, cujo
   procedimento está documentado na seção 9.2 mas não foi aplicado nas medições
   da seção 4. Afeta a coluna de ticks e a estatística de máximo, não a coluna
   de ciclos, que já exclui o kernel da contagem.
5. Resolvida na etapa 8. Os ciclos passaram a ser medidos, e a derivação da
   etapa 7 ficou validada contra eles.
6. O `exclude_kernel` exclui as interrupções da contagem. Os números da seção 4
   são o custo do programa isolado, que é o comparando certo para uma análise
   estática, mas não é o tempo que a tarefa consome numa aplicação real.

## 8. Etapa 8, concluída

Leitura de `PMCCNTR_EL0`, o contador de ciclos da unidade de monitoramento de
desempenho. Ele tica na frequência do núcleo, o que dá 44 vezes mais resolução
que o generic timer e mede diretamente a grandeza que o paper reporta, em vez de
derivá-la de uma medição de tempo.

Há dois caminhos para o mesmo contador, e o `measure_wcet.cpp` escolhe sozinho.

O primeiro é `mrs pmccntr_el0`, uma instrução, o mais barato possível. Ele exige
um módulo de kernel que escreva `PMUSERENR_EL0`, porque o acesso a partir de EL0
vem bloqueado. O módulo está em `etapa8_pmu/` e não pôde ser usado nesta placa,
cujo kernel `6.12.73-v8-16k+` foi compilado à mão e cujo diretório de build
aponta para uma árvore ausente. Nenhum pacote de headers casa com esse kernel.

O segundo é `perf_event_open(PERF_COUNT_HW_CPU_CYCLES)`, que abre o mesmo
contador de hardware pelo próprio kernel e não exige módulo nenhum. Foi o
caminho usado. A objeção óbvia, a de que um `read()` custa mais que os
benchmarks curtos, é resolvida por `exclude_kernel=1`, que faz a PMU parar de
contar enquanto a CPU está em EL1, deixando o custo da chamada de sistema fora
da contagem.

### Validação da conversão da seção 5.1

Os dois instrumentos foram lidos na mesma janela, o que permite confrontar o
valor medido com o derivado. A comparação só faz sentido depois de descontar o
piso de cada instrumento, que são bem diferentes: cerca de 44 ciclos na janela
de ticks contra cerca de 180 na de ciclos, já que esta última envolve a primeira
por fora e engole as duas leituras com seus `isb`.

| benchmark | medido (líquido) | derivado (líquido) | razão |
|---|---|---|---|
| matmult | 115.327 | 115.333 | 0,9999 |
| bsort100 | 72.785 | 72.800 | 0,9998 |
| crc | 1.971 | 2.000 | 0,9855 |
| ud | 1.891 | 1.911 | 0,9895 |
| fft1 | 1.778 | 1.800 | 0,9878 |
| statemate | 495 | 489 | 1,0125 |

Todas dentro de 1,5 %, e abaixo de 1 nos casos intermediários, que é o esperado
já que o `exclude_kernel` retira tempo de kernel que os ticks contabilizam. A
conversão `ticks x f_núcleo / f_timer` da etapa 7 fica validada por medição
direta nos seis benchmarks.

## 9. Como reproduzir

### 9.1 O que o programa já garante sozinho

Antes de medir qualquer coisa, o `measure_wcet.cpp` faz quatro coisas.

Prende a thread num núcleo só, com `sched_setaffinity`. Isso é obrigatório e
não é refinamento, porque o contador de ciclos é separado em cada núcleo e os
quatro não estão sincronizados entre si. Se o programa pulasse de núcleo entre a
leitura inicial e a final, a diferença entre as duas seria lixo, possivelmente
negativo. O programa confere em que núcleo terminou e reclama se não for o
pedido.

Sobe a prioridade para `SCHED_FIFO` 80, que é uma promessa do sistema
operacional de não interromper este programa para dar vez a outro de prioridade
normal.

Trava a memória na RAM com `mlockall`, para que o sistema não mova nenhuma
página para o disco no meio do lote.

Lê o governor e a frequência do núcleo antes e depois, e deixa a coluna de
ciclos vazia se a frequência mudou durante o lote.

As quatro juntas viram a coluna `isolamento_completo` do CSV. Ela vale 1 quando
todas deram certo, e uma linha com 0 ali não deve ser reportada.

### 9.2 O que ainda falta, e é opcional

A afinidade coloca o programa no núcleo 3, mas não impede o sistema operacional
de colocar outras coisas lá também. A diferença é a de sentar numa mesa vazia do
restaurante contra reservar a mesa. Enquanto ninguém mais chega, as duas
situações são idênticas, e quando alguém chega só a segunda protege.

Reservar o núcleo de verdade é feito no arranque da placa, não pelo programa.
São três instruções passadas ao kernel.

`isolcpus=3` tira o núcleo 3 da lista de núcleos onde o escalonador pode pôr
trabalho. Só vai para lá quem pedir explicitamente, que é o nosso caso.

`nohz_full=3` desliga a interrupção periódica de relógio naquele núcleo enquanto
houver uma única tarefa rodando. Sem isso o núcleo é interrompido centenas de
vezes por segundo mesmo sem ter nada para fazer.

`rcu_nocbs=3` move para outros núcleos um trabalho de faxina interna do kernel
que normalmente cai em todos.

Na Pi 5 e na Pi 4 o procedimento é o mesmo. Acrescente as três ao arquivo
`/boot/firmware/cmdline.txt`, que **precisa continuar sendo uma única linha**.
Quebrar essa linha em duas é o erro clássico, e a placa não arranca depois dele.
Em sistemas mais antigos que o Raspberry Pi OS Bookworm o arquivo fica em
`/boot/cmdline.txt`.

```sh
sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline.txt.bak
# edite e acrescente, na mesma linha:  isolcpus=3 nohz_full=3 rcu_nocbs=3
sudo reboot
```

Depois do arranque, confira se as três pegaram de verdade. Elas dependem de
opções de compilação do kernel, e um kernel sem `CONFIG_NO_HZ_FULL` aceita o
`nohz_full=3` em silêncio e não faz nada com ele.

```sh
cat /proc/cmdline                       # as tres devem aparecer aqui
dmesg | grep -i "dynticks\|isolat"      # confirma que o kernel obedeceu
```

Falta ainda afastar as interrupções de hardware do núcleo reservado.

```sh
for f in /proc/irq/*/smp_affinity_list; do echo 0-2 | sudo tee "$f"; done 2>/dev/null
```

Boa parte delas vai recusar a escrita, e isso é esperado. Interrupções presas a
um núcleo específico por construção do hardware não podem ser movidas. O que o
laço faz é mover as que podem.

**Quanto isso muda os números.** Menos do que parece, e vale saber disso antes
de mexer no arranque de uma placa que funciona. A contagem de ciclos usa
`exclude_kernel=1`, que já faz a PMU parar de contar enquanto a CPU está
atendendo interrupção. As interrupções portanto já estão fora da coluna de
ciclos, que é a coluna reportada. O que o isolamento no arranque melhora é a
coluna de ticks e a estatística de máximo, onde a seção 5.3 mostrou que as
excursões eram tempo de sistema operacional e não variação do programa.

O custo é que a placa fica com três núcleos utilizáveis em vez de quatro, e
compilar nela passa a ser mais lento.

Se o lote for rodado sem esse isolamento, o que é uma escolha legítima, o
relatório precisa dizer isso, e não deixar implícito.

### 9.3 O lote

Na placa, dentro de `wcet_bench/cycle_counter/`.

```sh
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# etapa 7, generic timer, ciclos derivados
sudo make medir  N=100 W=5 CSV=resultados_n100.csv

# ciclos medidos pela PMU. Gera a tabela da secao 4.
sudo make medir_wcet N=100 W=5 CSVW=resultados_pmu.csv
```

O `echo performance` é o que trava a velocidade do processador. Sem ele o
governor age como um câmbio automático, acelerando e desacelerando durante o
lote, e a seção 5.7 mostra que isso sozinho vale um fator 1,6 entre duas rodadas
do mesmo benchmark.

O `medir_wcet` escolhe sozinho como chegar no contador de ciclos e registra a
escolha na coluna `fonte_ciclos`. Nesta placa ela sai como `perf`. Se o módulo
de `etapa8_pmu/` estiver carregado, sai como `mrs` e a leitura fica mais barata,
sem mudar os valores.

O `stdout` de cada binário é uma linha de CSV e o `stderr` é o relatório
legível, de modo que redirecionar a tabela para arquivo não perde os avisos de
tela.
