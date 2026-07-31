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

ETAPAS := etapa1 etapa2 etapa3 etapa3-O2

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

clean:
	rm -f $(ETAPAS)
