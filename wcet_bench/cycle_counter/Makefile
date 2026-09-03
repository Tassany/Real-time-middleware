# Tutorial: medir tempo de execucao em ciclos de CPU (ARMv8-A / Raspberry Pi 5).
#
# Um alvo por etapa. A partir da etapa 5 passa a existir tambem um binario por
# benchmark, construido a partir das fontes Malardalen em ../ (que nunca sao
# modificadas).
#
#   make etapa1 && ./etapa1
#
# Este Makefile cresce a cada etapa do tutorial.

# ---------------------------------------------------------------------------
# Guarda de arquitetura.
#
# Todo o tutorial depende de instrucoes e registradores que so' existem em
# ARMv8-A (mrs, cntvct_el0, isb). Num x86 o compilador aceitaria alguns
# arquivos e rejeitaria outros com mensagens obscuras sobre operandos de
# assembly. Melhor falhar aqui, dizendo o que esta' errado.
# ---------------------------------------------------------------------------
ARCH := $(shell uname -m)
ifneq ($(ARCH),aarch64)
$(error Este tutorial e' AArch64 (Raspberry Pi 5 ou Pi 4 com sistema de 64 \
bits). Aqui uname -m diz '$(ARCH)'. Numa Pi 4 com Raspberry Pi OS de 32 bits \
isto diria 'armv7l' e tambem nao serve. Copie o diretorio wcet_bench inteiro \
para a placa e compile la')
endif

CXX      ?= g++

# -O0 ate' a etapa 3. Otimizacao muda o que esta' sendo medido -- inclusive
# apaga o codigo medido por inteiro, que e' justamente o assunto da etapa 3.
# Ate' la', o compilador faz o que o codigo-fonte manda, e nada mais.
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g

ETAPAS := etapa1 etapa2 etapa3 etapa3-O2 etapa4

.PHONY: all clean
all: $(ETAPAS)

etapa1: etapa1.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

etapa2: etapa2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# A etapa 3 e' o unico ponto do tutorial com duas versoes do mesmo fonte.
# O defeito 3 (o compilador apagar o codigo medido) so' existe com otimizacao
# ligada, entao voce precisa das duas para ver o antes e o depois.
etapa3: etapa3.cpp
	$(CXX) $(CXXFLAGS) -DNIVEL_OPT='"-O0"' -o $@ $<

etapa3-O2: etapa3.cpp
	$(CXX) -std=c++17 -Wall -Wextra -O2 -g -DNIVEL_OPT='"-O2"' -o $@ $<

etapa4: etapa4.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# ---------------------------------------------------------------------------
# Etapa 5: um binario por benchmark, em bin/.
#
#   bin/<nome> = obj/<nome>.o  (a fonte Malardalen em ../<nome>.c)
#              + obj/arn_<nome>.o  (etapa5.cpp, com -DBENCH_NAME)
#
# As fontes em ../ nunca sao modificadas. Toda a adaptacao esta' nas flags.
# ---------------------------------------------------------------------------
# As 34 fontes Malardalen que sao medives. Sao as 37 de ../ menos tres, que
# ficam de fora por defeito da propria fonte e nao do harness:
#
#   st.c     tem um `*/` orfao na linha 9, sobra de uma edicao de cabecalho.
#            Nao compila.
#   des.c    faz `#include <config.h>` e mais types.h, errors.h e des.h.
#            Nenhum dos quatro esta' em ../, e o pacote Malardalen daqui nao
#            os traz.
#   sqrt.c   nao tem main. Define so' sqrtfcn e fabs, e' arquivo de biblioteca.
#            Medi-lo exigiria escrever um driver, que seria codigo NOSSO dentro
#            da regiao medida.
#
# Corrigir as duas primeiras seria editar fonte Malardalen, o que esta' fora da
# regra deste diretorio. Elas ficam documentadas aqui e no RELATORIO.md.
BENCHES := adpcm bs bsort100 cnt compress cover crc duff edn expint \
           fac fdct fft1 fibcall fir insertsort janne_complex jfdctint \
           lcdnum lms ludcmp matmult minver ndes ns nsichneu prime \
           qsort-exam qurt recursion select statemate ud whet
SRCDIR  := ..
# Atribuicao simples, e nao ?=, porque o make ja' vem com CC=cc predefinido e
# um ?= nunca teria efeito. Passar CC=... na linha de comando continua valendo.
CC      = gcc

# -std=gnu89        as fontes sao de 2000-2005 e usam construcoes que o C
#                   moderno REJEITA: bsort100.c define funcoes no estilo K&R
#                   (`Initialize(Array) int Array[];`) e declara `main()` com
#                   tipo de retorno implicito. Em gnu89 as duas coisas sao
#                   legais. Nao e' preciosismo, sem isso nao compila.
# -O0               convencao da area de WCET, e a razao esta' na etapa 3:
#                   otimizacao muda (e as vezes apaga) o codigo medido.
# -fno-builtin      fft1.c define os proprios sin, cos, log e fabs estaticos.
#                   Sem esta flag o GCC pode substitui-los pelas versoes dele e
#                   voce mede um programa diferente do que esta' no arquivo.
# -fno-stack-protector
#                   Debian compila o GCC com -fstack-protector-strong ligado por
#                   padrao, o que injeta carga e comparacao de canario em toda
#                   funcao com vetor local. Isso e' codigo do compilador dentro
#                   da regiao medida, ausente do fonte e ausente do grafo que um
#                   analisador estatico enxerga. Desligar mantem o programa
#                   medido fiel ao programa escrito.
# -w                silencia os avisos das fontes antigas, que sao muitos e
#                   enterrariam qualquer aviso vindo do harness.
# -Dmain=bench_entry
#                   renomeia o main() do benchmark sem tocar no arquivo.
BENCH_CFLAGS := -std=gnu89 -O0 -g -fno-builtin -fno-stack-protector -w \
                -Dmain=bench_entry

# Defines especificos de cada fonte.
#   WCSIM        bsort100.c: silencia os printf do relatorio
#   WORSTCASE    bsort100.c: inicializa o vetor em ordem INVERTIDA, que e' o
#                pior caso do bubble sort e o unico que faz sentido num
#                experimento de WCET
#   UPPSALAWCET  matmult.c: silencia os printf e o #include de <sys/times.h>
# crc.c e fft1.c ja' guardam os printf deles atras de #ifdef DEBUG.
# Um write() dentro da regiao medida custaria mais que o benchmark inteiro.
DEFS_bsort100 := -DWCSIM -DWORSTCASE
DEFS_matmult  := -DUPPSALAWCET

# Defines do lado do harness, tambem por benchmark.
#   MAPEAR_ENDERECO_FIXO  bsort100.c le' o endereco absoluto 0x80200001,
#                         herdado de um ambiente sem sistema operacional. Num
#                         processo Linux isso e' SIGSEGV. O harness mapeia a
#                         pagina antes de rodar. Ver o comentario em etapa5.cpp.
#   DEFINIR_IN            recursion.c declara `extern volatile int In;` e escreve
#                         `In = fib(10)` no fim do main, sem que ninguem defina
#                         In. E' o sumidouro que impede o resultado de ser
#                         descartado, e quem tem que fornece-lo e' o harness.
#                         Sem isto o link falha com "undefined reference to In".
HDEFS_bsort100  := -DMAPEAR_ENDERECO_FIXO
HDEFS_recursion := -DDEFINIR_IN

BINS5 := $(addprefix bin5/,$(BENCHES))
BINS6 := $(addprefix bin6/,$(BENCHES))
BINS7 := $(addprefix bin7/,$(BENCHES))
BINSW := $(addprefix bin_wcet/,$(BENCHES))

.PHONY: etapa5 etapa6 etapa7 measure_wcet
etapa5: $(BINS5)
etapa6: $(BINS6)
etapa7: $(BINS7)
measure_wcet: $(BINSW)

bin5 bin6 bin7 bin_wcet obj:
	@mkdir -p $@

# O objeto da fonte Malardalen e' o mesmo para as duas etapas: quem muda e' o
# harness linkado junto.
obj/%.o: $(SRCDIR)/%.c | obj
	$(CC) $(BENCH_CFLAGS) $(DEFS_$*) -c -o $@ $<

obj/arn5_%.o: etapa5.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

obj/arn6_%.o: etapa6.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

obj/arn7_%.o: etapa7.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

obj/measure_%.o: measure_wcet.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

.SECONDARY: $(addprefix obj/,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/arn5_,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/arn6_,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/arn7_,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/measure_,$(addsuffix .o,$(BENCHES)))

bin5/%: obj/%.o obj/arn5_%.o | bin5
	$(CXX) -o $@ $^ -lm

bin6/%: obj/%.o obj/arn6_%.o | bin6
	$(CXX) -o $@ $^ -lm

bin7/%: obj/%.o obj/arn7_%.o | bin7
	$(CXX) -o $@ $^ -lm

bin_wcet/%: obj/%.o obj/measure_%.o | bin_wcet
	$(CXX) -o $@ $^ -lm

# ---------------------------------------------------------------------------
# A bateria completa: roda os 34 e junta numa tabela.
#
#   sudo make medir            n=5 (o numero do paper), 1 aquecimento
#   sudo make medir N=100 W=5  mais amostras e mais aquecimento
#
# O relatorio de cada benchmark continua indo para a tela (stderr); so' a linha
# de CSV entra no arquivo (stdout).
# ---------------------------------------------------------------------------
N ?= 5
W ?= 1
CSV ?= resultados.csv

.PHONY: medir
medir: $(BINS7)
	@./bin7/$(firstword $(BENCHES)) --header > $(CSV)
	@for b in $(BENCHES); do ./bin7/$$b -n $(N) -w $(W) >> $(CSV); done
	@echo
	@echo "==> $(CSV)"
	@column -s, -t $(CSV) 2>/dev/null || cat $(CSV)

# A mesma bateria com o contador de ciclos real:
#
#   sudo make medir_wcet N=100 W=5
#
# Os binarios escolhem sozinhos como chegar no contador, e dizem qual usaram na
# coluna `fonte_ciclos` do CSV:
#
#   mrs    PMCCNTR_EL0 direto, uma instrucao. Exige o modulo de etapa8_pmu/
#          carregado (cd etapa8_pmu && make load).
#   perf   perf_event_open, sem modulo nenhum. E' a fonte usada quando os
#          headers do kernel nao estao disponiveis para construir o modulo.
#
# Sem nenhuma das duas os binarios ainda rodam e preenchem so' as colunas da
# etapa 7. O sudo continua valendo por causa do SCHED_FIFO e do mlockall.
CSVW ?= resultados_pmu.csv

.PHONY: medir_wcet
medir_wcet: $(BINSW)
	@./bin_wcet/$(firstword $(BENCHES)) --header > $(CSVW)
	@for b in $(BENCHES); do ./bin_wcet/$$b -n $(N) -w $(W) >> $(CSVW); done
	@echo
	@echo "==> $(CSVW)"
	@column -s, -t $(CSVW) 2>/dev/null || cat $(CSVW)

clean:
	rm -f $(ETAPAS) $(CSV) $(CSVW)
	rm -rf bin bin5 bin6 bin7 bin_wcet obj
