# Build do modulo de kernel pmu_el0.
#
# Este Makefile e' diferente de todos os outros do tutorial. Modulos de kernel
# nao sao compilados por um comando gcc solto: eles precisam das MESMAS flags,
# dos MESMOS headers e da MESMA configuracao com que o kernel em execucao foi
# construido, senao o modulo nao carrega (o kernel recusa com "invalid module
# format" ou "version magic mismatch").
#
# O mecanismo que garante isso e' o kbuild. A linha `obj-m += pmu_el0.o` nao e'
# uma regra de make comum, e' uma declaracao lida pelo sistema de build do
# proprio kernel: "existe um modulo chamado pmu_el0 a construir". Quem faz o
# trabalho e' o Makefile do kernel, invocado logo abaixo.
#
#   -C $(KDIR)   entra no diretorio de build do kernel e usa o Makefile de la'
#   M=$(PWD)     diz a ele que o codigo-fonte do modulo esta' aqui
#
# Pre-requisito: os headers do kernel EM EXECUCAO, em /lib/modules/$(uname -r)/build.
#
# O nome do pacote mudou ao longo das versoes do Raspberry Pi OS. O antigo
# `raspberrypi-kernel-headers` nao existe mais no Bookworm em diante. Hoje os
# pacotes seguem o sabor do kernel:
#
#   linux-headers-rpi-2712     kernel especifico da Pi 5 (BCM2712)
#   linux-headers-rpi-v8       kernel arm64 generico
#   linux-headers-$(uname -r)  em distribuicoes que nao a Raspberry Pi OS
#
# Descubra o que existe com `apt-cache search linux-headers`.
#
# CASO SEM SAIDA: se o `uname -r` termina em `+` e nao contem `+rpt-`, o kernel
# em execucao NAO veio do apt (foi instalado por rpi-update ou compilado a mao).
# Nenhum pacote de headers vai casar com ele, porque o pacote e' gerado junto
# com o kernel empacotado. As alternativas sao voltar para o kernel do apt ou
# abandonar o modulo e ler os ciclos por perf_event_open.

obj-m += pmu_el0.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean load unload status

all:
	@test -d $(KDIR) || { \
		echo "erro: $(KDIR) nao existe, faltam os headers do kernel em uso."; \
		echo "      kernel: $(shell uname -r)"; \
		echo; \
		echo "      Procure o pacote certo:"; \
		echo "        apt-cache search linux-headers"; \
		echo "      Na Raspberry Pi OS costuma ser um destes:"; \
		echo "        sudo apt install linux-headers-rpi-2712   # Pi 5"; \
		echo "        sudo apt install linux-headers-rpi-v8     # arm64 generico"; \
		echo; \
		echo "      Se o kernel acima termina em '+' e nao tem '+rpt-', ele nao"; \
		echo "      veio do apt e nenhum pacote de headers vai casar. Veja o"; \
		echo "      comentario no topo deste Makefile."; \
		exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	sudo insmod pmu_el0.ko
	@dmesg | tail -3

unload:
	sudo rmmod pmu_el0
	@dmesg | tail -2

status:
	@lsmod | grep -q '^pmu_el0' \
		&& echo "pmu_el0: carregado" \
		|| echo "pmu_el0: NAO carregado"
