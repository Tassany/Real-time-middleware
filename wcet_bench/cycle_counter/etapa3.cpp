/*
 * etapa3.cpp — por que a medicao da etapa 2 mente, e como consertar.
 *
 * A etapa 2 le' o registrador certo e faz a subtracao certa. Mesmo assim a
 * medicao tem tres defeitos, e nenhum deles aparece como erro de compilacao ou
 * como valor obviamente absurdo. Eles aparecem como numeros plausiveis e
 * errados, que e' o pior tipo de bug num experimento.
 *
 *   DEFEITO 1  a CPU executa fora de ordem, entao o `mrs` pode acontecer fora
 *              da janela que voce achou que estava cercando.
 *   DEFEITO 2  o compilador reordena o codigo em volta das leituras.
 *   DEFEITO 3  o compilador APAGA o codigo que voce esta' medindo.
 *
 * E no fim uma pergunta que nenhuma etapa anterior fez: quanto custa o proprio
 * instrumento? Se ler o relogio custa 2 ticks, medir algo que dura 3 ticks e'
 * fantasia.
 *
 * Compile e rode as DUAS versoes, e compare:
 *
 *     make etapa3    && ./etapa3        (sem otimizacao, -O0)
 *     make etapa3-O2 && ./etapa3-O2     (com otimizacao, -O2)
 *
 * O defeito 3 so' se manifesta na segunda.
 */

#include <cstdio>
#include <cstdint>

#ifndef NIVEL_OPT
#define NIVEL_OPT "?"
#endif

/* ================================================================== */
/* DEFEITO 1 — execucao fora de ordem                                 */
/* ================================================================== */
/*
 * O Cortex-A76 da Pi 5 e' um nucleo SUPERESCALAR E FORA DE ORDEM. Ele nao
 * executa suas instrucoes na ordem em que voce as escreveu. Ele busca dezenas
 * de instrucoes adiante, descobre quais nao dependem umas das outras, e as
 * despacha para varias unidades de execucao em paralelo, conforme cada uma fica
 * livre. A ordem do programa e' preservada apenas no RESULTADO visivel, nao na
 * execucao.
 *
 * Isso e' otimo para desempenho e pessimo para medicao. Considere:
 *
 *     t0 = mrs cntvct_el0     <-- nao depende de nada
 *     ... o alvo ...
 *     t1 = mrs cntvct_el0     <-- nao depende do alvo
 *
 * Nada liga o `mrs` ao codigo do alvo. Nenhum registrador em comum, nenhuma
 * dependencia de dados. Do ponto de vista do escalonador interno do nucleo,
 * essas tres coisas sao independentes, e ele pode legitimamente executar o
 * segundo `mrs` ENQUANTO o alvo ainda esta' rodando, ou o primeiro `mrs` depois
 * que o alvo ja' comecou.
 *
 * A CORRECAO: `isb`, Instruction Synchronization Barrier.
 *
 * O `isb` esvazia o pipeline. Ele obriga o nucleo a concluir tudo que foi
 * despachado antes dele, e so' entao voltar a buscar instrucoes. Colocado antes
 * de cada leitura, ele garante que a fronteira da medicao e' de verdade uma
 * fronteira.
 *
 * O `isb` e' CARO de proposito, umas poucas dezenas de ciclos, porque jogar
 * fora um pipeline cheio custa. Voce esta' trocando exatidao da fronteira por
 * um custo fixo -- e esse custo e' exatamente o que a medicao do PISO no fim
 * deste arquivo quantifica. E' um trade honesto: um custo fixo e conhecido e'
 * muito melhor que uma fronteira difusa e desconhecida.
 *
 * (Rigor: o `isb` ordena o fluxo de instrucoes, nao a conclusao de acessos a
 * memoria -- para isso existe o `dsb`. Nao usamos `dsb` aqui porque as regioes
 * que vamos medir duram milissegundos, e alguns stores pendentes na fronteira
 * sao ruido irrelevante nessa escala.)
 */

/* ================================================================== */
/* DEFEITO 2 — reordenamento pelo compilador                          */
/* ================================================================== */
/*
 * O `volatile` da etapa 2 impede o compilador de APAGAR ou DUPLICAR o bloco
 * asm. Ele nao impede o compilador de MOVER outras coisas atraves dele.
 *
 * Da' para o compilador, o `asm volatile("mrs %0, cntvct_el0" : "=r"(v))`
 * declara: leio nada, escrevo v. Se ele quiser adiantar um par de stores do
 * alvo para antes do primeiro `mrs`, ou atrasa'-los para depois do segundo,
 * nada no bloco asm diz que isso e' proibido.
 *
 * A CORRECAO: o clobber "memory".
 *
 *     asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
 *                                                          ^^^^^^^^^^
 *
 * "memory" e' uma declaracao ao compilador de que este bloco asm pode ter lido
 * ou escrito qualquer posicao de memoria. A consequencia pratica: ele precisa
 * ter concluido todos os stores pendentes antes do bloco, e precisa reler da
 * memoria tudo que usar depois. Nenhum acesso a memoria atravessa a barreira.
 *
 * Repare que "memory" nao emite nenhuma instrucao. E' uma promessa quebrada de
 * proposito, uma restricao imposta ao compilador em tempo de compilacao, custo
 * zero em tempo de execucao. Diferente do `isb`, que e' hardware e custa.
 *
 * Sobre o "\n\t" no meio da string: o bloco asm e' texto colado no arquivo
 * assembly de saida, entao voce mesma precisa separar as duas instrucoes com
 * nova linha. O \t e' so' indentacao, para o assembly gerado ficar legivel.
 */

static inline uint64_t ler_ingenua()
{
	uint64_t v;

	/* Exatamente como na etapa 2: sem isb, sem clobber. */
	asm volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static inline uint64_t ler_correta()
{
	uint64_t v;

	asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}

static inline uint64_t ler_cntfrq()
{
	uint64_t v;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* ================================================================== */
/* DEFEITO 3 — o compilador apaga o que voce esta' medindo             */
/* ================================================================== */
/*
 * Este e' o mais perigoso dos tres, porque o sintoma e' um numero OTIMO.
 *
 * Um compilador otimizador tem licenca para eliminar qualquer computacao cujo
 * resultado nao seja observavel. Se voce ordena um vetor local, nunca le' o
 * resultado, e a funcao retorna void, entao ordenar ou nao ordenar da' no
 * mesmo, e o -O2 remove o laco inteiro. Nao sobra nada entre as duas leituras
 * do contador. A medicao cai para o custo de ler o contador duas vezes, e voce
 * conclui que a Pi 5 ordena 1024 elementos em 40 nanossegundos.
 *
 * Isso NAO e' um caso raro de laboratorio. E' precisamente o que acontece com
 * os benchmarks Malardalen que voce vai medir na etapa 5: eles calculam e
 * descartam. Por isso a convencao na area de WCET e' compilar essas fontes com
 * -O0, e por isso o Makefile deste tutorial usa -O0.
 *
 * A CORRECAO: `nao_otimize()`.
 *
 *     asm volatile("" : : "r"(p) : "memory");
 *
 * Um bloco asm VAZIO. Nao emite instrucao nenhuma, custo zero em execucao. Mas
 * ele diz duas mentiras uteis ao compilador:
 *
 *   "r"(p)     "preciso do ponteiro p num registrador" -- entao o endereco do
 *              vetor escapou, e o compilador nao pode mais provar que ninguem
 *              olha para o conteudo;
 *   "memory"   "este bloco pode ter lido qualquer memoria" -- entao todos os
 *              stores do laco precisam ter realmente acontecido antes daqui.
 *
 * Juntas, elas tornam o resultado do laco observavel, e o laco deixa de ser
 * removivel. O truque e' o mesmo que bibliotecas de benchmark como a do Google
 * usam, sob o nome DoNotOptimize.
 */

static inline void nao_otimize(void *p)
{
	asm volatile("" : : "r"(p) : "memory");
}

/* ---------------------------------------------------------------- alvos */

static const int N = 1024;

/* Este vetor e' global e o main() imprime dois elementos dele no fim. Portanto
 * o resultado E' observavel e o compilador nunca pode remover a ordenacao.
 * E' o nosso alvo de referencia, o mesmo das etapas 1 e 2. */
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

/* Mesmo trabalho, mas sobre um vetor LOCAL cujo resultado ninguem le'. E' a
 * situacao dos benchmarks Malardalen. `proteger` liga ou desliga o
 * nao_otimize() para voce ver os dois comportamentos lado a lado. */
static void alvo_descartado(bool proteger)
{
	int local[N];

	for (int i = 0; i < N; i++)
		local[i] = N - i;

	for (int i = 1; i < N; i++) {
		int chave = local[i];
		int j = i - 1;

		while (j >= 0 && local[j] > chave) {
			local[j + 1] = local[j];
			j--;
		}
		local[j + 1] = chave;
	}

	if (proteger)
		nao_otimize(local);
	/* sem `proteger`, `local` morre aqui sem nunca ter sido lido */
}

/* ================================================================== */
/* O PISO DO INSTRUMENTO                                              */
/* ================================================================== */
/*
 * Quanto custa medir? Duas leituras seguidas, sem absolutamente nada no meio.
 * A diferenca entre elas e' o custo do proprio par de leituras.
 *
 * Tiramos o MINIMO de mil tentativas, nao a media. Motivo: o ruido do sistema
 * (uma interrupcao, o escalonador) so' consegue ADICIONAR tempo, nunca subtrair.
 * Entao a menor amostra e' a menos contaminada, e a melhor estimativa do custo
 * intrinseco. Media misturaria custo com ruido.
 *
 * Esse numero e' a resolucao efetiva da sua regua. Qualquer benchmark que dure
 * poucos multiplos dele nao pode ser medido com honestidade -- guarde isso para
 * a etapa 5, onde o `crc` e o `ud` sao curtos.
 */

static uint64_t piso_ingenua()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t a = ler_ingenua();
		uint64_t b = ler_ingenua();

		if (b - a < menor)
			menor = b - a;
	}
	return menor;
}

static uint64_t piso_correta()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t a = ler_correta();
		uint64_t b = ler_correta();

		if (b - a < menor)
			menor = b - a;
	}
	return menor;
}

/* ------------------------------------------------------------------ main */

static double ns_por_tick;

static void linha(const char *rotulo, uint64_t ticks)
{
	printf("  %-34s %8llu ticks  %10.3f ms\n",
	       rotulo, (unsigned long long)ticks, ticks * ns_por_tick / 1e6);
}

int main()
{
	uint64_t f_timer = ler_cntfrq();

	ns_por_tick = f_timer ? 1e9 / (double)f_timer : 0.0;

	printf("etapa3 — compilado com %s, CNTFRQ_EL0 = %.3f MHz (%.2f ns/tick)\n\n",
	       NIVEL_OPT, f_timer / 1e6, ns_por_tick);

	/* ---- o piso do instrumento ---- */
	printf("PISO DO INSTRUMENTO (minimo de 1000 pares de leituras vazias)\n");
	linha("leitura ingenua (etapa 2)", piso_ingenua());
	linha("leitura correta (isb + memory)", piso_correta());
	printf("\n");

	/* ---- o alvo observavel, com os dois instrumentos ---- */
	printf("ALVO OBSERVAVEL (vetor global, o compilador nunca pode apagar)\n");

	preparar();
	uint64_t a0 = ler_ingenua();
	alvo();
	uint64_t a1 = ler_ingenua();
	linha("medido com a leitura ingenua", a1 - a0);

	preparar();
	uint64_t b0 = ler_correta();
	alvo();
	uint64_t b1 = ler_correta();
	linha("medido com a leitura correta", b1 - b0);
	printf("\n");

	/* ---- o alvo descartavel: a demonstracao do defeito 3 ---- */
	printf("ALVO DESCARTADO (vetor local, resultado nunca lido)\n");

	uint64_t c0 = ler_correta();
	alvo_descartado(false);
	uint64_t c1 = ler_correta();
	linha("sem nao_otimize()", c1 - c0);

	uint64_t d0 = ler_correta();
	alvo_descartado(true);
	uint64_t d1 = ler_correta();
	linha("com nao_otimize()", d1 - d0);
	printf("\n");

	printf("  vetor[0]=%d  vetor[%d]=%d  (ordenado: %s)\n",
	       vetor[0], N - 1, vetor[N - 1],
	       (vetor[0] == 1 && vetor[N - 1] == N) ? "sim" : "NAO");

	return 0;
}
