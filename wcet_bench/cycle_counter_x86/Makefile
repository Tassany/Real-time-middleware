# Porte para x86_64 de ../cycle_counter/Makefile. Mesmo padrao de vinculo por
# benchmark; unica diferenca estrutural e' a guarda de arquitetura e o
# harness (measure_wcet.cpp deste diretorio, sem os alvos etapa1..etapa7,
# que so' fazem sentido no tutorial ARM ao lado).
#
#   make measure_wcet && sudo make medir_wcet N=100 W=5

# ---------------------------------------------------------------------------
# Guarda de arquitetura.
#
# Este harness usa perf_event_open() e clock_gettime(), que sao Linux
# genericos, mas os BENCH_CFLAGS abaixo (-fno-builtin, -fno-stack-protector,
# -std=gnu89) so' foram validados em x86_64. Falhar cedo, com uma mensagem
# clara, e' melhor que deixar o make prosseguir numa arquitetura nao testada.
# ---------------------------------------------------------------------------
ARCH := $(shell uname -m)
ifneq ($(ARCH),x86_64)
$(error Este diretorio e' o porte x86_64 do harness de WCET. Aqui uname -m \
diz '$(ARCH)'. Para ARMv8-A use ../cycle_counter (aarch64) em vez deste \
diretorio)
endif

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g

# As mesmas 34 fontes Malardalen candidatas de ../cycle_counter/Makefile,
# menos os mesmos 5 excluidos (documentados la' e em ../cycle_counter_x86,
# problema de FONTE e nao de arquitetura):
#
#   st.c, des.c           nao compilam (erro de sintaxe / headers ausentes)
#   sqrt.c                nao tem main(), e' arquivo de biblioteca
#   select.c, qsort-exam.c  escrevem fora dos limites de um vetor global e
#                           corrompem o descritor perf_fd quando ligados
BENCHES := adpcm bs bsort100 cnt compress cover crc duff edn expint \
           fac fdct fft1 fibcall fir insertsort janne_complex jfdctint \
           lcdnum lms ludcmp matmult minver ndes ns nsichneu prime \
           qurt recursion statemate ud whet
SRCDIR  := ..
CC      = gcc

# Ver ../cycle_counter/Makefile para a justificativa linha a linha destas
# flags — sao sobre as fontes Malardalen (C89, sem builtins do compilador,
# sem canario de pilha), nao sobre a arquitetura.
BENCH_CFLAGS := -std=gnu89 -O0 -g -fno-builtin -fno-stack-protector -w \
                -Dmain=bench_entry

DEFS_bsort100 := -DWCSIM -DWORSTCASE
DEFS_matmult  := -DUPPSALAWCET

HDEFS_bsort100  := -DMAPEAR_ENDERECO_FIXO
HDEFS_recursion := -DDEFINIR_IN

BINSW := $(addprefix bin_wcet/,$(BENCHES))

.PHONY: all measure_wcet clean
all: measure_wcet
measure_wcet: $(BINSW)

bin_wcet obj:
	@mkdir -p $@

obj/%.o: $(SRCDIR)/%.c | obj
	$(CC) $(BENCH_CFLAGS) $(DEFS_$*) -c -o $@ $<

obj/measure_%.o: measure_wcet.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

.SECONDARY: $(addprefix obj/,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/measure_,$(addsuffix .o,$(BENCHES)))

bin_wcet/%: obj/%.o obj/measure_%.o | bin_wcet
	$(CXX) -o $@ $^ -lm

# ---------------------------------------------------------------------------
# A bateria completa: roda os benchmarks e junta numa tabela.
#
#   sudo make medir_wcet            n=5 (o numero do paper), 1 aquecimento
#   sudo make medir_wcet N=100 W=5  mais amostras e mais aquecimento
#
# O relatorio de cada benchmark vai para a tela (stderr); so' a linha de CSV
# entra no arquivo (stdout). sudo e' necessario para SCHED_FIFO e mlockall;
# sem ele o harness ainda roda e diz, na coluna isolamento_completo, que o
# isolamento ficou parcial.
# ---------------------------------------------------------------------------
N ?= 5
W ?= 1
CSVW ?= resultados_pmu_x86.csv

.PHONY: medir_wcet
medir_wcet: $(BINSW)
	@./bin_wcet/$(firstword $(BENCHES)) --header > $(CSVW)
	@for b in $(BENCHES); do ./bin_wcet/$$b -n $(N) -w $(W) >> $(CSVW); done
	@echo
	@echo "==> $(CSVW)"
	@column -s, -t $(CSVW) 2>/dev/null || cat $(CSVW)

clean:
	rm -f $(CSVW)
	rm -rf bin_wcet obj
