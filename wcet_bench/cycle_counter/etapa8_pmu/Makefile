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
# Pre-requisito na Raspberry Pi OS:
#
#   sudo apt install raspberrypi-kernel-headers
#
# Em outras distribuicoes o pacote costuma se chamar linux-headers-$(uname -r).
# Se /lib/modules/$(uname -r)/build nao existir, e' isso que esta' faltando.

obj-m += pmu_el0.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all clean load unload status

all:
	@test -d $(KDIR) || { \
		echo "erro: $(KDIR) nao existe."; \
		echo "      Instale os headers do kernel:"; \
		echo "      sudo apt install raspberrypi-kernel-headers"; \
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
