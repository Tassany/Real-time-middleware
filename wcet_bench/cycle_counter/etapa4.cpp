/*
 * etapa4.cpp — o protocolo do paper: aquecer, repetir 5 vezes, tirar a media.
 *
 * A etapa 3 deixou o instrumento honesto. Falta o procedimento em volta dele.
 * O paper diz, em resumo:
 *
 *     "Cinco tempos de execucao medidos sao promediados para obter o WCET
 *      observado final."
 *
 * Este arquivo implementa exatamente isso, e mais tres coisas que o paper nao
 * menciona e sem as quais o numero nao significa o que parece significar:
 *
 *   A) restaurar o estado de entrada antes de cada execucao;
 *   B) descartar uma execucao de aquecimento;
 *   C) reportar minimo, mediana e maximo junto da media.
 *
 * Uso:  ./etapa4 [n]     (n = execucoes medidas, padrao 5 como no paper)
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

/* ------------------------------------------- instrumento (vem da etapa 3) */

static inline uint64_t ler_contador()
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

static inline void nao_otimize(void *p)
{
	asm volatile("" : : "r"(p) : "memory");
}

/* ------------------------------------------------------------------ alvo */

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

/* ================================================================== */
/* A) RESTAURAR O ESTADO ANTES DE CADA EXECUCAO                       */
/* ================================================================== */
/*
 * O erro mais facil de cometer nesta etapa, e o mais dificil de perceber
 * depois:
 *
 *     for (i = 0; i < 5; i++) {
 *         t0 = ler_contador();
 *         alvo();                  // <-- ERRADO
 *         t1 = ler_contador();
 *     }
 *
 * A primeira execucao ordena o vetor. A segunda recebe um vetor JA' ORDENADO,
 * que e' o MELHOR caso do insertion sort: o laco interno nunca entra, e o custo
 * cai de O(n^2) para O(n). Voce mediria uma vez o pior caso e quatro vezes o
 * melhor, e reportaria a media disso como "WCET observado". O numero sairia
 * cinco vezes menor que a verdade, sem nenhum sinal de erro.
 *
 * A correcao e' chamar preparar() antes de cada execucao. E ela precisa ficar
 * FORA da janela medida, senao voce estaria cronometrando a preparacao junto
 * com o alvo.
 *
 * Isso vale para os benchmarks Malardalen da etapa 5 tambem, e la' e' mais
 * sutil: varios deles trabalham sobre vetores globais inicializados uma vez
 * so'. Rodar bench_entry() cinco vezes seguidas nao mede a mesma coisa cinco
 * vezes.
 *
 * Um limite honesto desta correcao: preparar() restaura o CONTEUDO do vetor,
 * nao o estado da maquina. Depois da primeira execucao o vetor esta' na cache
 * L1, o preditor de saltos ja' aprendeu o padrao do laco, e as paginas ja'
 * estao mapeadas. Restaurar de verdade o estado inicial de um processador e'
 * um problema bem maior, e neste tutorial nao vamos resolve-lo -- o que da'
 * para fazer e' saber que ele existe e nao alegar mais do que se mediu.
 */

/* ================================================================== */
/* B) AQUECIMENTO                                                      */
/* ================================================================== */
/*
 * A primeira execucao de qualquer coisa e' sistematicamente mais lenta, e nao
 * por causa do algoritmo. Ela paga:
 *
 *   - CACHE FRIA. O vetor de 4 KB ainda esta' na RAM. Cada linha de cache
 *     custa uma ida a' memoria na primeira passada e nada nas seguintes.
 *   - FALTAS DE PAGINA. O Linux entrega memoria preguicosamente. As paginas do
 *     vetor global so' ganham RAM fisica de verdade no primeiro toque, e cada
 *     primeiro toque e' uma excecao tratada pelo kernel.
 *   - RESOLUCAO PREGUICOSA DE SIMBOLOS. Chamadas a funcoes de biblioteca
 *     dinamica passam por um trampolim que, na primeira vez, invoca o linker
 *     dinamico para descobrir o endereco de verdade.
 *   - PREDITOR DE SALTOS VAZIO. Ele ainda nao viu este laco e erra mais.
 *
 * Nada disso e' propriedade do codigo que voce quer medir. Entao roda-se uma
 * vez e joga-se fora.
 *
 * Vale registrar a tensao aqui, porque ela e' relevante para o seu trabalho:
 * descartar o aquecimento produz numeros mais LIMPOS e menos PESSIMISTAS. Um
 * WCET de verdade teria que considerar a execucao de cache fria, que e' a mais
 * lenta. Aquecer aproxima o regime permanente, nao o pior caso. Como o paper
 * tambem promedia (o que ja' aponta para o caso medio), aquecer e' coerente com
 * o que ele faz -- mas nao com o nome "WCET".
 */

/* ================================================================== */
/* C) MEDIA NAO E' PIOR CASO                                          */
/* ================================================================== */
/*
 * "WCET" quer dizer Worst-Case Execution Time, tempo de execucao do PIOR CASO.
 * A media de cinco amostras estima o caso MEDIO. Sao estimadores de coisas
 * diferentes, e a media e' sistematicamente menor. Quanto mais voce repete,
 * mais a media converge para o centro da distribuicao e mais ela se AFASTA do
 * maximo -- ou seja, repetir mais piora a estimativa do pior caso.
 *
 * Este programa reporta a media, para reproduzir o paper fielmente, e imprime
 * ao lado:
 *
 *   MINIMO   a execucao menos perturbada. E' o melhor palpite sobre quanto o
 *            codigo custa quando nada mais acontece na maquina.
 *   MEDIANA  robusta a um ponto fora da curva. Se uma das cinco execucoes levou
 *            uma interrupcao, a media sobe e a mediana nao. A distancia entre
 *            media e mediana e' um detector barato de contaminacao.
 *   MAXIMO   o proxy honesto de pior caso. Nao e' o WCET (nada obtido por
 *            medicao e', porque voce nunca sabe se viu a pior entrada e o pior
 *            estado de cache), mas e' um limite inferior observado para ele, e
 *            e' o unico dos quatro que responde a pergunta certa.
 *
 * Cinco amostras e' pouco para qualquer afirmacao estatistica. E' o que o paper
 * faz, entao e' o padrao aqui, mas o programa aceita um argumento para voce
 * rodar com 100 ou 1000 e ver a diferenca.
 */

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	int n = 5;                 /* o numero do paper */
	const int aquecimentos = 1;

	if (argc > 1) {
		n = atoi(argv[1]);
		if (n < 1 || n > 100000) {
			fprintf(stderr, "uso: %s [n]   com 1 <= n <= 100000\n", argv[0]);
			return 2;
		}
	}

	uint64_t f_timer = ler_cntfrq();
	double ns_por_tick = f_timer ? 1e9 / (double)f_timer : 0.0;

	uint64_t *amostras = new uint64_t[n];

	/* ---- aquecimento: executa e joga fora ---- */
	for (int i = 0; i < aquecimentos; i++) {
		preparar();
		alvo();
		nao_otimize(vetor);
	}

	/* ---- as n execucoes medidas ---- */
	for (int i = 0; i < n; i++) {
		preparar();                        /* fora da janela, de proposito */

		uint64_t t0 = ler_contador();
		alvo();
		uint64_t t1 = ler_contador();

		nao_otimize(vetor);
		amostras[i] = t1 - t0;
	}

	/* ---- estatisticas ---- */
	uint64_t *ordenadas = new uint64_t[n];

	std::copy(amostras, amostras + n, ordenadas);
	std::sort(ordenadas, ordenadas + n);

	double soma = 0.0;
	for (int i = 0; i < n; i++)
		soma += (double)ordenadas[i];

	double media   = soma / n;
	uint64_t minimo = ordenadas[0];
	uint64_t maximo = ordenadas[n - 1];
	double mediana = (n % 2)
		? (double)ordenadas[n / 2]
		: ((double)ordenadas[n / 2 - 1] + (double)ordenadas[n / 2]) / 2.0;

	/* ---- relatorio ---- */
	printf("etapa4 — insertion sort, %d elementos\n", N);
	printf("  %d execucoes medidas, %d de aquecimento descartada(s)\n",
	       n, aquecimentos);
	printf("  CNTFRQ_EL0 = %.3f MHz (%.2f ns/tick)\n\n",
	       f_timer / 1e6, ns_por_tick);

	printf("AMOSTRAS\n");
	for (int i = 0; i < n && i < 20; i++)
		printf("  execucao %2d   %8llu ticks   %8.3f ms\n",
		       i + 1, (unsigned long long)amostras[i],
		       amostras[i] * ns_por_tick / 1e6);
	if (n > 20)
		printf("  ... (%d amostras restantes omitidas)\n", n - 20);
	printf("\n");

	printf("RESUMO\n");
	printf("  minimo    %10.1f ticks   %8.3f ms\n",
	       (double)minimo, minimo * ns_por_tick / 1e6);
	printf("  mediana   %10.1f ticks   %8.3f ms\n",
	       mediana, mediana * ns_por_tick / 1e6);
	printf("  MEDIA     %10.1f ticks   %8.3f ms   <- \"WCET observado\" do paper\n",
	       media, media * ns_por_tick / 1e6);
	printf("  maximo    %10.1f ticks   %8.3f ms   <- proxy honesto de pior caso\n",
	       (double)maximo, maximo * ns_por_tick / 1e6);
	printf("\n");

	/* Dois numeros de diagnostico que dizem se o resumo acima merece
	 * confianca. Ambos deveriam ser pequenos numa maquina isolada, e a etapa 6
	 * e' inteiramente sobre faze-los encolher. */
	printf("DIAGNOSTICO\n");
	printf("  dispersao (max-min)/mediana   %6.1f %%\n",
	       mediana > 0 ? 100.0 * (double)(maximo - minimo) / mediana : 0.0);
	printf("  media/mediana                 %6.3f    (>1 sugere amostra contaminada)\n",
	       mediana > 0 ? media / mediana : 0.0);
	printf("  maximo/media                  %6.3f    (o quanto a media subestima)\n",
	       media > 0 ? (double)maximo / media : 0.0);

	delete[] ordenadas;
	delete[] amostras;
	return 0;
}
