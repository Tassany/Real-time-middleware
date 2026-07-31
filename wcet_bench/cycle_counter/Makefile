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
$(error Este tutorial e' AArch64 (Raspberry Pi 5). Aqui uname -m diz '$(ARCH)'. \
Copie o diretorio wcet_bench inteiro para a Pi e compile la')
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
BENCHES := bsort100 crc fft1 matmult ud statemate
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
HDEFS_bsort100 := -DMAPEAR_ENDERECO_FIXO

BINS5 := $(addprefix bin5/,$(BENCHES))
BINS6 := $(addprefix bin6/,$(BENCHES))

.PHONY: etapa5 etapa6
etapa5: $(BINS5)
etapa6: $(BINS6)

bin5 bin6 obj:
	@mkdir -p $@

# O objeto da fonte Malardalen e' o mesmo para as duas etapas: quem muda e' o
# harness linkado junto.
obj/%.o: $(SRCDIR)/%.c | obj
	$(CC) $(BENCH_CFLAGS) $(DEFS_$*) -c -o $@ $<

obj/arn5_%.o: etapa5.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

obj/arn6_%.o: etapa6.cpp | obj
	$(CXX) $(CXXFLAGS) -DBENCH_NAME='"$*"' $(HDEFS_$*) -c -o $@ $<

.SECONDARY: $(addprefix obj/,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/arn5_,$(addsuffix .o,$(BENCHES))) \
            $(addprefix obj/arn6_,$(addsuffix .o,$(BENCHES)))

bin5/%: obj/%.o obj/arn5_%.o | bin5
	$(CXX) -o $@ $^ -lm

bin6/%: obj/%.o obj/arn6_%.o | bin6
	$(CXX) -o $@ $^ -lm

clean:
	rm -f $(ETAPAS)
	rm -rf bin bin5 bin6 obj
