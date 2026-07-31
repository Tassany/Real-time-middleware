/*
 * etapa1.cpp — o esqueleto da medicao, ainda com um relogio comum.
 *
 * Objetivo desta etapa: estabelecer o padrao
 *
 *     ler o tempo  ->  executar o codigo  ->  ler o tempo  ->  subtrair
 *
 * que e' exatamente o que o paper descreve. Aqui ainda NAO tocamos em nenhum
 * registrador da CPU: usamos o relogio que o Linux oferece, clock_gettime().
 * Isso serve para duas coisas:
 *
 *   1. Voce ve o formato da medicao antes de ter que entender assembly.
 *   2. O numero que sai daqui e' o valor de referencia contra o qual vamos
 *      conferir o contador de ciclos nas etapas seguintes. Se la' na frente a
 *      conversao de ticks para microssegundos nao bater com este valor, e' a
 *      conversao que esta' errada.
 *
 * Compile e rode:  make etapa1 && ./etapa1
 */

#include <cstdio>
#include <cstdint>
#include <ctime>

/* ------------------------------------------------------------------ alvo */
/*
 * O "trecho de codigo a ser medido". Nas etapas 1 a 4 e' esta funcao de
 * brinquedo; na etapa 5 ela da' lugar aos benchmarks Malardalen de verdade.
 *
 * Insertion sort sobre um vetor ja' em ordem DECRESCENTE. Essa escolha nao e'
 * arbitraria: ordem invertida e' o pior caso do algoritmo, em que todo elemento
 * precisa atravessar a lista inteira. Sao ~1024*1023/2 = 523.776 comparacoes,
 * trabalho suficiente para o resultado ficar bem acima de qualquer ruido de
 * medicao.
 */

static const int N = 1024;
static int vetor[N];

static void preparar()
{
	for (int i = 0; i < N; i++)
		vetor[i] = N - i;   /* N, N-1, N-2, ..., 1 */
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

/* --------------------------------------------------------------- relogio */
/*
 * clock_gettime() pede um "clock id" e devolve um instante. Qual clock voce
 * escolhe muda tudo:
 *
 *   CLOCK_REALTIME  — a hora do mundo, 14:32 de terca-feira. E' o que time() e
 *                     std::chrono::system_clock usam. Esse relogio PODE ANDAR
 *                     PARA TRAS: o NTP corrige a hora da maquina de tempos em
 *                     tempos, e o horario de verao existe. Se um ajuste desses
 *                     cair entre as suas duas leituras, t1 - t0 da' negativo, ou
 *                     um valor absurdo. Nunca use para medir duracao.
 *
 *   CLOCK_MONOTONIC — um contador que so' cresce, contado desde algum instante
 *                     arbitrario do passado (tipicamente o boot). O valor
 *                     absoluto nao significa nada; a DIFERENCA entre duas
 *                     leituras significa tudo. E' o certo aqui.
 *
 * O struct timespec parte o instante em dois campos, segundos e nanossegundos,
 * porque um unico inteiro de 64 bits em nanossegundos estouraria em ~584 anos e
 * o padrao POSIX preferiu nao arriscar. Juntamos os dois num uint64_t, que
 * aguenta tranquilamente as duracoes que vamos medir.
 */

static uint64_t agora_ns()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ main */

int main()
{
	struct timespec res;

	preparar();

	/* --- a medicao propriamente dita: tres linhas, e e' isso --- */
	uint64_t t0 = agora_ns();
	alvo();
	uint64_t t1 = agora_ns();

	uint64_t duracao = t1 - t0;

	/* Resolucao do relogio: o menor incremento que ele consegue representar.
	 * Se a duracao medida fosse da mesma ordem desse numero, a medicao seria
	 * so' arredondamento. Nao e' o caso aqui, mas o habito de conferir a
	 * resolucao do instrumento antes de confiar nele volta na etapa 3. */
	clock_getres(CLOCK_MONOTONIC, &res);

	printf("etapa1 — insertion sort, %d elementos em ordem invertida\n\n", N);
	printf("  leitura antes  : %llu ns\n", (unsigned long long)t0);
	printf("  leitura depois : %llu ns\n", (unsigned long long)t1);
	printf("  diferenca      : %llu ns   (%.3f ms)\n",
	       (unsigned long long)duracao, duracao / 1e6);
	printf("\n");
	printf("  resolucao do CLOCK_MONOTONIC : %ld ns\n", (long)res.tv_nsec);

	/* Prova de que o trabalho aconteceu de verdade. Parece supersticao agora,
	 * mas na etapa 3 voce vai ver o compilador APAGAR a chamada a alvo() e a
	 * medicao despencar para quase zero. Ter uma evidencia independente de que
	 * o vetor foi mesmo ordenado e' o que permite perceber isso. */
	printf("  vetor[0]=%d  vetor[%d]=%d  (ordenado: %s)\n",
	       vetor[0], N - 1, vetor[N - 1],
	       (vetor[0] == 1 && vetor[N - 1] == N) ? "sim" : "NAO");

	return 0;
}
