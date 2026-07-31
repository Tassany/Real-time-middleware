/*
 * etapa5.cpp — o mesmo protocolo, agora sobre os benchmarks Malardalen.
 *
 * O instrumento (etapas 2 e 3) e o procedimento (etapa 4) estao prontos. Falta
 * trocar a funcao de brinquedo pela carga de verdade: bsort100, crc, fft1,
 * matmult, ud e statemate, cujas fontes estao em ../ e NAO sao modificadas.
 *
 * Um binario por benchmark:  bin/bsort100, bin/crc, ...
 *
 * Uso:  ./bin/<nome> [-n EXEC] [-w AQUECIMENTOS]
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef BENCH_NAME
#define BENCH_NAME "desconhecido"
#endif

/* ================================================================== */
/* O PROBLEMA: dois main() nao cabem num programa                     */
/* ================================================================== */
/*
 * Cada benchmark Malardalen e' um programa completo, com seu proprio main().
 * Este arquivo tambem tem um main(). Se voce simplesmente compilar os dois
 * juntos, o linker reclama de simbolo duplicado e para.
 *
 * COMPILACAO SEPARADA, primeiro, porque tudo aqui depende disso.
 *
 * Compilar e linkar sao duas etapas distintas. O compilador processa UM arquivo
 * por vez e produz um arquivo-objeto (.o) contendo codigo de maquina mais uma
 * tabela de simbolos: os nomes que aquele arquivo DEFINE e os que ele USA sem
 * definir. O linker vem depois, junta todos os .o e casa cada nome usado com
 * a sua definicao. Se um nome tem duas definicoes, ele nao sabe qual escolher.
 * Se um nome nao tem nenhuma, e' o famoso "undefined reference".
 *
 * A consequencia util: bsort100.c e etapa5.cpp nunca se veem. Cada um e'
 * compilado sozinho, e o unico contrato entre eles e' um nome de funcao.
 *
 * A SOLUCAO: renomear main() na linha de comando.
 *
 *     gcc -Dmain=bench_entry -c ../bsort100.c
 *
 * -D define uma macro de pre-processador, exatamente como um #define no topo do
 * arquivo, so' que sem tocar no arquivo. O pre-processador troca toda ocorrencia
 * do texto `main` por `bench_entry` antes do compilador ver o codigo. O
 * `main()` do bsort100 vira `bench_entry()`, uma funcao comum. O arquivo em
 * disco continua intacto, e e' por isso que este tutorial nao precisa de copias
 * modificadas das fontes.
 *
 * E O extern "C", que e' o pedaco que so' existe porque este arquivo e' C++.
 *
 * C++ permite sobrecarga: duas funcoes podem se chamar `f` se receberem tipos
 * diferentes. Para isso funcionar, o compilador C++ codifica os tipos dos
 * parametros dentro do nome do simbolo -- e' o NAME MANGLING. `void f(int)`
 * vira algo como `_Z1fi` no arquivo-objeto.
 *
 * O compilador C nao faz nada disso. `bench_entry` no bsort100.o e' literalmente
 * `bench_entry`.
 *
 * Se este arquivo declarasse `void bench_entry();` sem mais nada, o g++
 * procuraria `_Z11bench_entryv` e o linker falharia. `extern "C"` desliga o
 * mangling para aquela declaracao: procure o nome cru, como o C escreveria.
 *
 * DETALHE FINO, sobre o tipo de retorno.
 *
 * As seis fontes discordam entre si: fft1.c, matmult.c e ud.c declaram
 * `void main()`, enquanto crc.c e statemate.c declaram `int main(void)`, e
 * bsort100.c escreve so' `main()`, que em C89 significa retorno int implicito.
 *
 * Nao da' para declarar todos com um tipo so' e estar certa em todos os casos.
 * Declaramos como void e descartamos o retorno quando ele existe. Na convencao
 * de chamada do AArch64 o valor de retorno vem no registrador x0, e ignorar x0
 * e' inofensivo -- quem chama simplesmente nao le'. A alternativa (declarar int
 * e ler x0 mesmo quando a funcao nao escreveu nada nele) seria pior, porque
 * leria lixo.
 */

extern "C" void bench_entry();

/* ================================================================== */
/* bsort100: um endereco de memoria cravado na fonte                  */
/* ================================================================== */
/*
 * bsort100.c abre com isto:
 *
 *     #define KNOWN_VALUE (int)(*((char *)0x80200001))
 *
 * e usa a macro em Initialize(). Ou seja, o benchmark LE' O ENDERECO ABSOLUTO
 * 0x80200001 esperando encontrar o valor 1 la' dentro. O comentario da fonte
 * diz a intencao: "uma leitura deste endereco resultara' num valor conhecido
 * de 1".
 *
 * Isso funcionava no ambiente para o qual o benchmark foi escrito, um simulador
 * ou uma placa sem sistema operacional, onde o programa enxerga o mapa de
 * memoria fisica cru e aquele endereco tinha um valor fixo cravado.
 *
 * Num processo Linux nao existe mapa fisico. Cada processo tem um ESPACO DE
 * ENDERECAMENTO VIRTUAL proprio, e o kernel so' liga um endereco virtual a
 * memoria fisica quando alguem pede. Enderecos que ninguem pediu ficam sem
 * traducao, e le-los gera uma falha de pagina que o kernel nao sabe resolver.
 * O resultado e' SIGSEGV, que e' o "Segmentation fault" que voce viu.
 *
 * O conserto poderia ser editar a fonte e trocar a macro por 1. Nos nao vamos
 * fazer isso, porque a fonte intocada e' o que garante que voce esta' medindo o
 * mesmo programa que os outros trabalhos da area medem. Em vez disso, pedimos
 * ao kernel a pagina que falta:
 *
 *     mmap(0x80200000, 4096, ...) e escrevemos 1 no byte 1.
 *
 * mmap com MAP_FIXED_NOREPLACE pede um endereco ESPECIFICO, e falha em vez de
 * mover a mapeamento para outro lugar se aquela regiao ja' estiver ocupada. O
 * endereco precisa ser multiplo do tamanho da pagina, e 0x80200000 e'.
 * Depois disso, ler 0x80200001 devolve 1, exatamente como o benchmark supunha,
 * e o programa em disco continua intocado.
 */
#ifdef MAPEAR_ENDERECO_FIXO

#include <sys/mman.h>
#include <unistd.h>

/* MAP_FIXED_NOREPLACE existe a partir do Linux 4.17. Se a libc for antiga,
 * cai para MAP_FIXED, que faz a mesma coisa mas sobrescreve em silencio um
 * mapeamento existente em vez de reclamar. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

static void mapear_endereco_fixo()
{
	void *base = (void *)0x80200000UL;
	size_t tam = (size_t)sysconf(_SC_PAGESIZE);

	void *p = mmap(base, tam, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (p == MAP_FAILED || p != base) {
		fprintf(stderr,
			"erro: nao consegui mapear a pagina em 0x80200000 que o "
			"bsort100 le'.\n");
		exit(1);
	}

	unsigned char *b = (unsigned char *)p;

	b[1] = 1;   /* KNOWN_VALUE, o unico que bsort100 usa */
	b[3] = 1;   /* UNKNOWN_VALUE, definido na fonte mas nunca chamado */
}

#else
static void mapear_endereco_fixo() { }
#endif

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

static inline void barreira()
{
	asm volatile("" : : : "memory");
}

/* ================================================================== */
/* ESTADO ENTRE EXECUCOES — a licao A da etapa 4, agora de verdade    */
/* ================================================================== */
/*
 * Na etapa 4 nos chamavamos preparar() antes de cada execucao, para nao medir
 * quatro vezes o melhor caso. Aqui nao da': o estado esta' dentro do benchmark,
 * em variaveis globais e estaticas que este arquivo nem enxerga.
 *
 * O que cada uma das seis fontes faz, depois de ler o codigo delas:
 *
 *   bsort100   main() chama Initialize(), que reescreve o vetor inteiro em
 *              ordem invertida. Restaura-se sozinho. OK.
 *   matmult    main() chama InitSeed(), e Test() regenera as matrizes a partir
 *              da semente. Restaura-se sozinho. OK.
 *   ud         main() preenche a[][] e b[] no proprio corpo. OK.
 *   statemate  main() chama init(). OK.
 *
 *   crc        NAO. icrc() tem `static unsigned short icrctb[256], init=0;` e
 *              constroi a tabela de 256 entradas apenas na PRIMEIRA chamada.
 *              Da segunda em diante o `if (!init)` e' falso e o trabalho some.
 *              Logo: a execucao 1 e' muito mais cara que as execucoes 2..5, e
 *              se voce aquecer antes de medir, a construcao da tabela cai
 *              inteira no aquecimento e NENHUMA amostra medida a inclui.
 *
 *   fft1       PARCIALMENTE. main() reescreve ar[] a cada chamada, mas nao
 *              ai[], que fica com a saida da fft anterior. Da segunda execucao
 *              em diante o benchmark opera sobre dados diferentes. A estrutura
 *              de lacos do fft1 nao depende dos dados, entao o tempo tende a
 *              ser estavel, mas os valores nao sao os mesmos e vale conferir se
 *              a amostra 1 destoa.
 *
 * Nao ha' conserto limpo dentro deste desenho: zerar `init` do crc de fora e'
 * impossivel, porque e' uma estatica local. O conserto de verdade seria uma
 * medicao por processo, relancando o binario a cada amostra. Este programa em
 * vez disso torna o problema VISIVEL: com `-w 0` voce mede sem aquecimento e ve
 * a amostra 1 destoar; comparando com `-w 1` voce ve exatamente quanto da'
 * medicao o aquecimento levou embora.
 */

/* ------------------------------------------------------------------ main */

static void uso(const char *prog)
{
	fprintf(stderr,
		"uso: %s [-n EXEC] [-w AQUECIMENTOS]\n"
		"  -n EXEC           execucoes medidas (padrao 5, o numero do paper)\n"
		"  -w AQUECIMENTOS   execucoes descartadas antes (padrao 1)\n", prog);
}

int main(int argc, char **argv)
{
	int n = 5;
	int aquecimentos = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			aquecimentos = atoi(argv[++i]);
		} else {
			uso(argv[0]);
			return 2;
		}
	}
	if (n < 1 || n > 100000 || aquecimentos < 0) {
		uso(argv[0]);
		return 2;
	}

	uint64_t f_timer = ler_cntfrq();
	double ns_por_tick = f_timer ? 1e9 / (double)f_timer : 0.0;

	mapear_endereco_fixo();

	uint64_t *amostras = new uint64_t[n];

	for (int i = 0; i < aquecimentos; i++) {
		bench_entry();
		barreira();
	}

	for (int i = 0; i < n; i++) {
		uint64_t t0 = ler_contador();
		bench_entry();
		uint64_t t1 = ler_contador();

		barreira();
		amostras[i] = t1 - t0;
	}

	uint64_t *ordenadas = new uint64_t[n];

	std::copy(amostras, amostras + n, ordenadas);
	std::sort(ordenadas, ordenadas + n);

	double soma = 0.0;
	for (int i = 0; i < n; i++)
		soma += (double)ordenadas[i];

	double media    = soma / n;
	uint64_t minimo = ordenadas[0];
	uint64_t maximo = ordenadas[n - 1];
	double mediana  = (n % 2)
		? (double)ordenadas[n / 2]
		: ((double)ordenadas[n / 2 - 1] + (double)ordenadas[n / 2]) / 2.0;

	printf("etapa5 — benchmark \"%s\"\n", BENCH_NAME);
	printf("  %d execucoes medidas, %d de aquecimento\n", n, aquecimentos);
	printf("  CNTFRQ_EL0 = %.3f MHz (%.2f ns/tick)\n\n",
	       f_timer / 1e6, ns_por_tick);

	printf("AMOSTRAS\n");
	for (int i = 0; i < n && i < 20; i++)
		printf("  execucao %2d   %10llu ticks   %10.4f ms\n",
		       i + 1, (unsigned long long)amostras[i],
		       amostras[i] * ns_por_tick / 1e6);
	if (n > 20)
		printf("  ... (%d amostras restantes omitidas)\n", n - 20);
	printf("\n");

	printf("RESUMO\n");
	printf("  minimo    %12.1f ticks   %10.4f ms\n",
	       (double)minimo, minimo * ns_por_tick / 1e6);
	printf("  mediana   %12.1f ticks   %10.4f ms\n",
	       mediana, mediana * ns_por_tick / 1e6);
	printf("  MEDIA     %12.1f ticks   %10.4f ms   <- \"WCET observado\"\n",
	       media, media * ns_por_tick / 1e6);
	printf("  maximo    %12.1f ticks   %10.4f ms   <- proxy de pior caso\n",
	       (double)maximo, maximo * ns_por_tick / 1e6);
	printf("\n");

	printf("DIAGNOSTICO\n");
	printf("  dispersao (max-min)/mediana   %6.1f %%\n",
	       mediana > 0 ? 100.0 * (double)(maximo - minimo) / mediana : 0.0);
	printf("  media/mediana                 %6.3f\n",
	       mediana > 0 ? media / mediana : 0.0);

	/* O problema nao e' so' o piso do instrumento (2 ticks), e' a
	 * QUANTIZACAO. A menor divisao da regua vale 1 tick. Se a medida inteira
	 * cabe em poucas dezenas de ticks, o erro de arredondamento e' uma fatia
	 * enorme do resultado, e os digitos finais do "WCET observado" sao ruido
	 * de discretizacao, nao propriedade do programa. */
	double res_rel = mediana > 0 ? 100.0 / mediana : 0.0;

	printf("  resolucao relativa (1 tick)   %6.2f %%\n", res_rel);

	if (mediana < 100.0)
		printf("  AVISO: mediana de %.1f ticks. A regua de %.2f ns nao "
		       "resolve este benchmark;\n"
		       "         a dispersao acima e' majoritariamente quantizacao. "
		       "Numero NAO utilizavel.\n", mediana, ns_por_tick);
	else if (mediana < 1000.0)
		printf("  ATENCAO: mediana de %.1f ticks da' so' %.2f %% de "
		       "resolucao. Use com ressalva.\n", mediana, res_rel);

	delete[] ordenadas;
	delete[] amostras;
	return 0;
}
