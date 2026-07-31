/*
 * etapa2.cpp — ler um registrador da CPU direto, com assembly inline.
 *
 * Mesma medicao da etapa 1, mesmo alvo, mesma subtracao. So' muda o
 * instrumento: em vez de pedir a hora ao Linux, lemos o registrador
 * CNTVCT_EL0 do proprio processador.
 *
 * Esse e' o registrador que o paper usa.
 *
 * AVISO, para voce nao confiar demais no numero que sai daqui: esta leitura
 * ainda esta' errada de tres maneiras diferentes, e a etapa 3 conserta as tres.
 * Por enquanto o objetivo e' so' conseguir ler o registrador e entender o que
 * cada pedaco da sintaxe faz.
 *
 * Compile e rode:  make etapa2 && ./etapa2
 */

#include <cstdio>
#include <cstdint>

/* ------------------------------------------------------------------ alvo */
/* Identico ao da etapa 1, para o numero ser comparavel. */

static const int N = 1024;
static int vetor[N];

static void preparar()
{
	for (int i = 0; i < N; i++)
		vetor[i] = N - i;
}

static void alvo()
{
	for (int i = 1; i < N; i++) {
		int chave = vetor[i];
		int j = i - 1;

		while (j >= 0 && vetor[j] > chave) {
			vetor[j + 1] = vetor[j];
			j--;
		}
		vetor[j + 1] = chave;
	}
}

/* -------------------------------------------------- lendo o registrador */
/*
 * PRIMEIRO: o que e' um registrador.
 *
 * Uma variavel comum mora na memoria RAM. O processador, para usa'-la, emite um
 * acesso ao barramento e espera -- dezenas ou centenas de ciclos se o dado nao
 * estiver na cache. Um REGISTRADOR e' diferente: e' um punhado de biestaveis
 * dentro do proprio nucleo, sem endereco de memoria, acessivel em um ciclo. O
 * AArch64 tem 31 registradores de uso geral (x0..x30) que o compilador usa o
 * tempo todo sem voce perceber, mais um conjunto separado de REGISTRADORES DE
 * SISTEMA, que nao guardam dados do seu programa e sim estado e configuracao do
 * hardware. CNTVCT_EL0 e' um destes.
 *
 * SEGUNDO: por que C++ nao consegue le'-lo.
 *
 * Nao existe endereco para apontar. `int *p = (int *)CNTVCT_EL0;` nao quer
 * dizer nada, porque o registrador nao esta' no espaco de enderecamento. A
 * unica forma de chegar nele e' a instrucao dedicada
 *
 *     mrs <destino>, <registrador de sistema>      "Move from System Register"
 *
 * (a inversa e' `msr`, que escreve). C++ nao tem sintaxe para emitir uma
 * instrucao especifica, entao voce precisa de uma valvula de escape: ou um
 * "intrinsic" que o compilador reconheca, ou ASSEMBLY INLINE. Para este
 * registrador nao ha' intrinsic portavel, entao vai ser assembly inline.
 *
 * TERCEIRO: dissecando a linha.
 *
 *     asm volatile("mrs %0, cntvct_el0" : "=r"(v));
 *      |     |       |          |          |  |  |
 *      |     |       |          |          |  |  +-- a variavel C++ envolvida
 *      |     |       |          |          |  +----- "r": ponha-a num registrador
 *      |     |       |          |          |         de uso geral qualquer
 *      |     |       |          |          +-------- "=": e' SO' escrita; o valor
 *      |     |       |          |                    anterior de v nao importa
 *      |     |       |          +------------------- o registrador de sistema
 *      |     |       +------------------------------ %0 = o operando numero 0,
 *      |     |                                       ou seja, o "=r"(v) abaixo.
 *      |     |                                       O compilador escolhe QUAL
 *      |     |                                       registrador fisico usar e
 *      |     |                                       substitui aqui.
 *      |     +-------------------------------------- o texto assembly, colado
 *      |                                             tal e qual na saida
 *      +-------------------------------------------- "volatile": nao apague nem
 *                                                     mova esta instrucao. Sem
 *                                                     isso o compilador ve uma
 *                                                     conta sem efeito colateral
 *                                                     e pode reaproveitar UMA
 *                                                     leitura para as duas
 *                                                     chamadas -- e a diferenca
 *                                                     daria exatamente zero.
 *
 * QUARTO: por que voce tem PERMISSAO de fazer isso sem sudo.
 *
 * O ARM define quatro NIVEIS DE EXCECAO, que sao os anéis de privilegio da
 * arquitetura:
 *
 *     EL0  o seu programa
 *     EL1  o kernel do Linux
 *     EL2  hipervisor
 *     EL3  firmware seguro
 *
 * O sufixo no nome do registrador diz o nivel MINIMO que pode toca'-lo.
 * CNTVCT_EL0 termina em _EL0, entao em principio o seu programa alcanca. Mas
 * "em principio" e' pouco: o acesso ainda passa por um bit de habilitacao,
 * CNTKCTL_EL1.EL0VCTEN, que so' o kernel pode ligar. O Linux liga, e nao por
 * generosidade -- ele precisa disso para o vDSO, o mecanismo que faz
 * clock_gettime() (a etapa 1!) responder sem entrar no kernel. Voce esta'
 * pegando carona nessa decisao.
 *
 * Nem todo registrador tem essa sorte. Na etapa 8 vamos tentar ler
 * PMCCNTR_EL0, que e' o contador de ciclos DE VERDADE, e o programa vai morrer
 * com "Illegal instruction" -- porque o bit de habilitacao equivalente
 * (PMUSERENR_EL0) vem desligado, e liga'-lo exige um modulo de kernel.
 */

static inline uint64_t ler_cntvct()
{
	uint64_t v;

	asm volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

/*
 * CNTFRQ_EL0 nao conta nada; e' um registrador de configuracao, gravado pelo
 * firmware no boot, que diz A QUE FREQUENCIA o contador acima incrementa.
 * Sem ele os "ticks" do CNTVCT sao um numero puro sem unidade.
 */
static inline uint64_t ler_cntfrq()
{
	uint64_t v;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* ------------------------------------------------------------------ main */

int main()
{
	preparar();

	uint64_t f_timer = ler_cntfrq();

	/* --- a mesma medicao da etapa 1, com o instrumento trocado --- */
	uint64_t t0 = ler_cntvct();
	alvo();
	uint64_t t1 = ler_cntvct();

	uint64_t ticks = t1 - t0;

	printf("etapa2 — insertion sort, %d elementos, medido com CNTVCT_EL0\n\n", N);

	printf("  CNTFRQ_EL0 : %llu Hz", (unsigned long long)f_timer);
	if (f_timer == 0) {
		printf("   <<< ZERO: o firmware nao programou o registrador.\n");
		printf("               Sem isso os ticks nao tem unidade.\n");
	} else {
		printf("   (%.3f MHz)\n", f_timer / 1e6);
		/* Resolucao do instrumento: o intervalo de tempo que UM tick
		 * representa. Compare com o 1 ns do CLOCK_MONOTONIC da etapa 1 --
		 * este relogio de hardware e' MUITO mais grosseiro, e essa e' a
		 * primeira surpresa desagradavel do tutorial. */
		printf("  um tick    : %.2f ns   (a etapa 1 tinha resolucao de 1 ns)\n",
		       1e9 / (double)f_timer);
	}
	printf("\n");

	printf("  leitura antes  : %llu ticks\n", (unsigned long long)t0);
	printf("  leitura depois : %llu ticks\n", (unsigned long long)t1);
	printf("  diferenca      : %llu ticks\n", (unsigned long long)ticks);
	printf("\n");

	printf("  vetor[0]=%d  vetor[%d]=%d  (ordenado: %s)\n",
	       vetor[0], N - 1, vetor[N - 1],
	       (vetor[0] == 1 && vetor[N - 1] == N) ? "sim" : "NAO");

	/* Nao convertemos ticks em tempo aqui de proposito. A conversao parece
	 * uma divisao trivial e nao e': ela esconde a diferenca entre "ticks de
	 * um timer" e "ciclos de processador", que e' o ponto onde o paper
	 * escorrega. Isso e' a etapa 7, inteira. */

	return 0;
}
