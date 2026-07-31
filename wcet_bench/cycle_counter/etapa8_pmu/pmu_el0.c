// SPDX-License-Identifier: GPL-2.0
/*
 * pmu_el0.c — libera PMCCNTR_EL0 para leitura em espaco de usuario.
 *
 * ============================ POR QUE ISTO EXISTE ==========================
 *
 * O CNTVCT_EL0 das etapas 2 a 7 e' o generic timer: tica a 54 MHz fixos e mede
 * TEMPO. Um tick vale 44 ciclos de nucleo a 2,4 GHz, e por isso quatro dos seis
 * benchmarks nao tem resolucao utilizavel.
 *
 * O PMCCNTR_EL0 e' outra coisa. Ele pertence a PMU, a unidade de monitoramento
 * de desempenho do nucleo, e conta CICLOS DE NUCLEO de verdade, um a um. Isso
 * da' 44 vezes mais resolucao e mede a grandeza que o paper reporta, em vez de
 * uma grandeza derivada dela.
 *
 * Ele existe no Cortex-A76 e o nome termina em _EL0, entao em principio o seu
 * programa poderia le-lo. Na pratica nao pode: a leitura em EL0 fica bloqueada
 * ate' que alguem em EL1 (o kernel) autorize, escrevendo em PMUSERENR_EL0. Um
 * `mrs x0, pmccntr_el0` sem essa autorizacao gera uma excecao que o Linux
 * traduz em SIGILL, o "Illegal instruction".
 *
 * ========================= POR QUE VEM BLOQUEADO ===========================
 *
 * Nao e' arbitrariedade. Duas razoes concretas.
 *
 * A PMU e' um canal lateral. Um contador de ciclos de alta resolucao acessivel
 * sem privilegio e' o instrumento classico para ataques de temporizacao sobre
 * cache, do tipo que descobre chaves criptograficas observando quanto tempo
 * outro processo leva. Foi por isso que os navegadores reduziram a precisao de
 * performance.now() depois do Spectre.
 *
 * E a PMU e' por nucleo. Cada nucleo tem o seu contador, com seu proprio valor,
 * e nada garante que estejam sincronizados. Se a sua thread migrar no meio da
 * medicao, a diferenca entre as duas leituras e' lixo, e pode ate' ser negativa.
 * O CNTVCT_EL0 nao tem esse problema porque e' alimentado por um contador de
 * sistema unico e externo aos nucleos. Aqui, prender a thread num nucleo
 * (etapa 6) deixa de ser refinamento e passa a ser obrigatorio.
 *
 * ============================== O QUE ELE FAZ ==============================
 *
 * Em cada nucleo, quatro escritas:
 *
 *   PMCCFILTR_EL0 = 0      conte em todos os niveis de excecao. Se voce quisesse
 *                          contar so' o codigo do seu programa e ignorar o
 *                          tempo gasto dentro de interrupcoes, ligaria o bit 31
 *                          (P, exclui EL1). Deixamos contando tudo porque uma
 *                          interrupcao que rouba tempo do benchmark faz parte do
 *                          que voce quer enxergar.
 *
 *   PMCR_EL0               E=1 liga os contadores, C=1 zera o de ciclos,
 *                          LC=1 usa 64 bits, D=0 sem divisor. O LC importa: com
 *                          32 bits o contador daria a volta a cada 1,8 segundo
 *                          a 2,4 GHz. O D importa igualmente: com o divisor
 *                          ligado voce contaria de 64 em 64 ciclos e perderia
 *                          justamente a resolucao que veio buscar.
 *
 *   PMCNTENSET_EL0 bit 31  habilita especificamente o contador de ciclos.
 *
 *   PMUSERENR_EL0          EN (bit 0) mais CR (bit 2), que juntos liberam a
 *                          leitura a partir de EL0. E' a escrita que so' o
 *                          kernel pode fazer, e a razao de este arquivo ser um
 *                          modulo e nao mais uma funcao no etapa8.cpp.
 *
 * on_each_cpu() executa a funcao em todos os nucleos online, porque cada um tem
 * seu proprio conjunto desses registradores. Escrever so' no nucleo onde o
 * insmod calhou de rodar deixaria os outros tres bloqueados.
 *
 * =============================== CUIDADOS ==================================
 *
 * 1. Isto conflita com o perf. O kernel tem um driver de PMU (armv8_pmuv3) que
 *    gerencia esses mesmos registradores. Enquanto este modulo estiver
 *    carregado, `perf stat` e afins podem reprogramar o PMCR por baixo e voce
 *    ficar com contagens erradas. Nao use os dois ao mesmo tempo.
 *
 * 2. Nucleos que entrarem em linha depois do insmod nao serao configurados.
 *    Tratar isso direito exige registrar um callback de hotplug de CPU, o que
 *    nao foi feito aqui para manter o modulo legivel. Na Pi 5 os quatro nucleos
 *    ja' estao online no boot, entao na pratica nao aparece.
 *
 * 3. Enquanto carregado, qualquer processo do sistema pode ler o contador de
 *    ciclos. Descarregue com rmmod quando terminar de medir.
 *
 * ================================= USO =====================================
 *
 *   sudo apt install raspberrypi-kernel-headers   # ou linux-headers-$(uname -r)
 *   make
 *   sudo insmod pmu_el0.ko
 *   dmesg | tail -2
 *   ...                                           # rode as medicoes
 *   sudo rmmod pmu_el0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <asm/barrier.h>

/* PMCR_EL0 */
#define PMCR_E		(1UL << 0)	/* habilita todos os contadores        */
#define PMCR_C		(1UL << 2)	/* zera o contador de ciclos           */
#define PMCR_D		(1UL << 3)	/* conta de 64 em 64 ciclos (indesejado) */
#define PMCR_LC		(1UL << 6)	/* contador de ciclos com 64 bits      */

/* PMCNTENSET_EL0 / PMCNTENCLR_EL0 */
#define PMCNTEN_C	(1UL << 31)	/* o bit do contador de ciclos         */

/* PMUSERENR_EL0 */
#define PMUSERENR_EN	(1UL << 0)	/* acesso EL0 aos registradores da PMU */
#define PMUSERENR_CR	(1UL << 2)	/* leitura EL0 do contador de ciclos   */

static void habilitar_neste_nucleo(void *ignorado)
{
	u64 pmcr;

	/* conta em EL0 e EL1, sem filtro */
	asm volatile("msr pmccfiltr_el0, %0" : : "r"((u64)0));

	asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	pmcr |= PMCR_E | PMCR_C | PMCR_LC;
	pmcr &= ~PMCR_D;
	asm volatile("msr pmcr_el0, %0" : : "r"(pmcr));

	asm volatile("msr pmcntenset_el0, %0" : : "r"((u64)PMCNTEN_C));
	asm volatile("msr pmuserenr_el0, %0"
		     : : "r"((u64)(PMUSERENR_EN | PMUSERENR_CR)));

	/* As escritas em registrador de sistema so' valem depois de um isb. Sem
	 * ele, uma leitura logo em seguida pode ainda ver o estado antigo. */
	isb();
}

static void desabilitar_neste_nucleo(void *ignorado)
{
	u64 pmcr;

	/* Revoga primeiro o acesso de EL0, depois desliga. Na ordem inversa
	 * haveria uma janela em que EL0 leria um contador ja' parado. */
	asm volatile("msr pmuserenr_el0, %0" : : "r"((u64)0));
	asm volatile("msr pmcntenclr_el0, %0" : : "r"((u64)PMCNTEN_C));

	asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	pmcr &= ~PMCR_E;
	asm volatile("msr pmcr_el0, %0" : : "r"(pmcr));

	isb();
}

static int __init pmu_el0_init(void)
{
	/* o 1 final pede que on_each_cpu espere todos os nucleos terminarem */
	on_each_cpu(habilitar_neste_nucleo, NULL, 1);

	pr_info("pmu_el0: PMCCNTR_EL0 liberado para EL0 em %u nucleos\n",
		num_online_cpus());
	pr_info("pmu_el0: nao use perf enquanto este modulo estiver carregado\n");
	return 0;
}

static void __exit pmu_el0_exit(void)
{
	on_each_cpu(desabilitar_neste_nucleo, NULL, 1);
	pr_info("pmu_el0: acesso EL0 revogado, contador de ciclos desligado\n");
}

module_init(pmu_el0_init);
module_exit(pmu_el0_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Libera o contador de ciclos PMCCNTR_EL0 para leitura em EL0");
