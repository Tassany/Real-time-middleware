/*
 * etapa7.cpp — de ticks para tempo, de tempo para ciclos, e uma linha de CSV.
 *
 * A etapa 6 entregou medicoes reprodutiveis. Falta responder a pergunta que o
 * paper de fato faz, que e' em CICLOS DE PROCESSADOR, e nao em ticks nem em
 * microssegundos.
 *
 * O ponto central, e onde o paper e' impreciso:
 *
 *     CNTVCT_EL0 NAO CONTA CICLOS DE PROCESSADOR.
 *
 * Ele e' o generic timer. Tica a CNTFRQ_EL0, 54 MHz nesta placa, fixos, gravados
 * pelo firmware no boot. O nucleo, enquanto isso, roda a 1,5, 1,8 ou 2,4 GHz
 * conforme o governor decide. Sao dois relogios independentes.
 *
 * Consequencia, e a etapa 6 mostrou isso com dados:
 *
 *     ticks -> nanossegundos   e' EXATO.  ns = ticks * 1e9 / f_timer
 *                              O f_timer e' constante e conhecido.
 *
 *     ticks -> ciclos          e' DERIVADO.  ciclos = ticks * f_nucleo / f_timer
 *                              Depende de f_nucleo, que muda, e a formula supoe
 *                              que ele ficou parado durante todo o lote.
 *
 * A validacao empirica que voce ja' produziu, medindo o matmult em tres clocks:
 *
 *     1500 MHz -> 4156 ticks -> 115.444 ciclos
 *     1800 MHz -> 3459 ticks -> 115.300 ciclos
 *     2400 MHz -> 2595 ticks -> 115.333 ciclos
 *
 * Os ticks variam 38%, os ciclos concordam em 0,125%. A conversao funciona, e a
 * quantidade invariante e' a contagem de ciclos. Mas repare no que ela EXIGE:
 * saber f_nucleo, e que ele nao tenha se mexido. Por isso este programa le' a
 * frequencia antes e depois do lote e recusa a coluna de ciclos se ela mudou.
 *
 * NOVIDADE DE FORMATO: a saida se divide em dois canais.
 *
 *   stdout   uma linha de CSV, legivel por maquina, para juntar os seis
 *            benchmarks numa tabela so'
 *   stderr   o relatorio legivel por gente, igual ao das etapas anteriores
 *
 * Assim `./bin7/crc >> tabela.csv` funciona sem que o relatorio suje o arquivo,
 * e voce continua vendo os avisos na tela. E' o que o alvo `make medir` usa.
 *
 * Um binario por benchmark:  bin7/bsort100, bin7/crc, ...
 *
 * Uso:  ./bin7/<nome> [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <algorithm>

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* ================================================================== */
/* ISOLAMENTO 1 — prender a thread a um nucleo                        */
/* ================================================================== */
/*
 * Por padrao o escalonador do Linux pode mover sua thread de nucleo a qualquer
 * momento, e ele faz isso o tempo todo para equilibrar carga. Cada mudanca
 * custa caro na medicao:
 *
 *   - CACHE PERDIDA. A Pi 5 tem quatro Cortex-A76, cada um com sua L1 de 64 KiB
 *     e sua L2 de 512 KiB privadas. Ao migrar, sua thread chega num nucleo onde
 *     nada do seu programa esta' em cache, e recomeca fria.
 *   - PREDITOR DE SALTOS PERDIDO, pelo mesmo motivo.
 *
 * Uma coisa que a migracao NAO estraga, e vale saber por contraste: o valor do
 * CNTVCT_EL0. O generic timer do ARM e' alimentado por um contador de sistema
 * unico, externo aos nucleos, e a arquitetura exige que todos os nucleos vejam
 * o mesmo valor. Ler antes num nucleo e depois noutro continua dando uma
 * diferenca correta. No x86 o contador equivalente e' por nucleo e essa mesma
 * migracao produziria lixo, as vezes ate' negativo. Voce esta' com sorte de
 * arquitetura aqui.
 *
 * A CORRECAO: sched_setaffinity().
 *
 * A "afinidade" de uma thread e' o conjunto de nucleos onde ela pode rodar. Por
 * padrao sao todos. O tipo cpu_set_t e' uma mascara de bits, um bit por nucleo,
 * manipulada pelas macros CPU_ZERO (limpa tudo) e CPU_SET (liga um bit).
 * Reduzindo o conjunto a um unico nucleo, o escalonador nao tem mais para onde
 * levar a thread.
 *
 * Nao precisa de sudo: reduzir a propria afinidade e' permitido a qualquer
 * processo.
 *
 * O LIMITE DISSO: prender a SUA thread a um nucleo nao impede o resto do
 * sistema de usar aquele mesmo nucleo. Interrupcoes e outras tarefas continuam
 * chegando la'. O isolamento de verdade e' um parametro de boot,
 * `isolcpus=3 nohz_full=3 irqaffinity=0-2` no cmdline.txt, que retira o nucleo
 * 3 do escalonador comum e desvia as interrupcoes para os outros. Isso esta'
 * fora do escopo deste tutorial, mas e' o proximo passo se voce precisar de
 * numeros melhores que os que vamos obter aqui.
 */

static bool prender_no_nucleo(int nucleo)
{
	cpu_set_t conjunto;

	CPU_ZERO(&conjunto);
	CPU_SET(nucleo, &conjunto);

	return sched_setaffinity(0, sizeof(conjunto), &conjunto) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 2 — nao ser interrompida pelo escalonador               */
/* ================================================================== */
/*
 * Prender a thread num nucleo nao impede que ela seja TIRADA dele. A politica
 * padrao do Linux e' SCHED_OTHER, atendida pelo CFS, que reparte o tempo entre
 * todas as tarefas prontas. Sua thread ganha uma fatia, e quando a fatia acaba
 * ela sai e outra entra, mesmo que a sua tenha trabalho a fazer. Se isso cair
 * no meio da janela medida, o tempo da outra tarefa entra na sua medicao.
 *
 * A CORRECAO: SCHED_FIFO.
 *
 * O Linux tem politicas de TEMPO REAL, e SCHED_FIFO e' a mais simples delas:
 * uma thread FIFO roda ate' bloquear ou ceder voluntariamente, e so' pode ser
 * preemptada por outra thread de tempo real com prioridade MAIOR. Nenhuma
 * tarefa comum, de qualquer tipo, interrompe uma thread FIFO. As prioridades
 * vao de 1 a 99.
 *
 * EXIGE PRIVILEGIO (a capacidade CAP_SYS_NICE, na pratica sudo), e a razao e'
 * obvia: uma thread FIFO em laco infinito monopolizaria um nucleo. O Linux tem
 * uma rede de seguranca, a limitacao de banda de tempo real, que por padrao
 * concede no maximo 950 ms de cada 1000 ms as tarefas de tempo real
 * (/proc/sys/kernel/sched_rt_runtime_us). Ou seja, mesmo em FIFO voce leva uma
 * interrupcao forcada de 50 ms a cada segundo. Para lotes curtos como os
 * nossos isso raramente aparece.
 *
 * Se falhar, este programa DIZ que falhou e continua. Reportar um numero sem
 * avisar que o isolamento nao aconteceu seria pior do que nao ter tentado.
 */

static bool virar_tempo_real(int prioridade)
{
	struct sched_param p;

	memset(&p, 0, sizeof(p));
	p.sched_priority = prioridade;

	return sched_setscheduler(0, SCHED_FIFO, &p) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 3 — nenhuma falta de pagina dentro da janela            */
/* ================================================================== */
/*
 * O Linux entrega memoria preguicosamente. Quando seu programa reserva um
 * vetor, o kernel promete o espaco no espaco de enderecamento virtual mas nao
 * entrega RAM fisica. A entrega acontece no primeiro acesso, atraves de uma
 * FALTA DE PAGINA: o hardware percebe que aquele endereco virtual nao tem
 * traducao, gera uma excecao, o kernel arruma uma pagina fisica, atualiza a
 * tabela de paginas e devolve o controle. Custa microssegundos.
 *
 * Se uma falta dessas cair dentro da janela medida, ela aparece como tempo de
 * execucao do benchmark. E' o mesmo fenomeno que o aquecimento da etapa 4
 * combate, so' que aqui de forma explicita e permanente.
 *
 * A CORRECAO: mlockall(MCL_CURRENT | MCL_FUTURE).
 *
 *   MCL_CURRENT   traz para a RAM e fixa la' tudo que ja' esta' mapeado agora
 *   MCL_FUTURE    faz o mesmo automaticamente para tudo que for mapeado depois
 *
 * Fixado significa que o kernel nao pode mandar aquelas paginas para a swap nem
 * desfazer o mapeamento. Como consequencia, nenhum acesso a memoria do seu
 * processo pode gerar falta de pagina.
 *
 * Precisa de privilegio na pratica: o limite RLIMIT_MEMLOCK de um usuario comum
 * costuma ser de poucos megabytes, e como root e' ilimitado.
 */

static bool fixar_memoria()
{
	return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 4 — o clock do nucleo                                   */
/* ================================================================== */
/*
 * A maior fonte de erro que a etapa 5 revelou, e a unica que este programa NAO
 * conserta sozinho.
 *
 * O governor de frequencia decide, dezenas de vezes por segundo, a que clock
 * cada nucleo roda. Com `ondemand`, o padrao da Raspberry Pi OS, ele sobe
 * quando ve carga e desce quando nao ve. Um lote de cinco execucoes de poucos
 * microssegundos e' curto demais para ele reagir, entao o lote inteiro roda
 * no clock em que a maquina JA' ESTAVA -- que depende do que voce fez antes.
 * Foi isso que produziu a razao de 1,598 entre duas rodadas do matmult.
 *
 * Por que o programa nao arruma sozinho: escrever em scaling_governor muda o
 * comportamento da maquina inteira e continua valendo depois que o benchmark
 * termina. Um programa de medicao que altera o sistema em silencio e' pior que
 * um que so' reporta. Entao aqui nos LEMOS e DENUNCIAMOS.
 *
 * A leitura acontece ANTES e DEPOIS do lote. Se a frequencia mudou no meio, o
 * lote e' misturado e o programa avisa. Isso vai importar ainda mais na
 * etapa 7, onde converter ticks em ciclos exige justamente supor que o clock
 * ficou parado.
 */

static uint64_t ler_u64_sysfs(const char *caminho)
{
	FILE *f = fopen(caminho, "r");
	unsigned long long v = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	return (uint64_t)v;
}

/* Frequencia atual do nucleo, em Hz. O sysfs reporta em kHz. */
static uint64_t freq_nucleo_hz(int nucleo)
{
	char caminho[160];

	snprintf(caminho, sizeof(caminho),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", nucleo);
	return ler_u64_sysfs(caminho) * 1000ULL;
}

static void ler_governor(int nucleo, char *destino, size_t tam)
{
	char caminho[160];

	snprintf(caminho, sizeof(caminho),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", nucleo);

	FILE *f = fopen(caminho, "r");

	if (!f || !fgets(destino, (int)tam, f)) {
		snprintf(destino, tam, "?");
	} else {
		destino[strcspn(destino, "\n")] = '\0';
	}
	if (f)
		fclose(f);
}

/* ------------------------------------------------------------------ main */

static void uso(const char *prog)
{
	fprintf(stderr,
		"uso: %s [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]\n"
		"  -n EXEC           execucoes medidas (padrao 5, o numero do paper)\n"
		"  -w AQUECIMENTOS   execucoes descartadas antes (padrao 1)\n"
		"  -c NUCLEO         nucleo onde prender a thread (padrao 3)\n"
		"  --header          imprime so' o cabecalho do CSV e sai\n"
		"\n"
		"stdout leva uma linha de CSV; o relatorio legivel vai para stderr.\n"
		"Rode com sudo para conseguir SCHED_FIFO e mlockall.\n", prog);
}

/* As colunas do CSV. Ordem fixa: se voce precisar acrescentar alguma coisa
 * depois, ACRESCENTE NO FIM, para nao quebrar planilhas e scripts que ja'
 * dependem das posicoes atuais. */
static const char *CSV_CABECALHO =
	"benchmark,n,aquecimentos,nucleo,governor,"
	"f_nucleo_hz,f_timer_hz,clock_estavel,isolamento_completo,"
	"min_ticks,mediana_ticks,media_ticks,max_ticks,"
	"media_us,max_us,"
	"media_ciclos,mediana_ciclos,max_ciclos,"
	"resolucao_rel_pct,dispersao_pct,utilizavel";

int main(int argc, char **argv)
{
	int n = 5;
	int aquecimentos = 1;
	int nucleo = 3;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--header")) {
			puts(CSV_CABECALHO);
			return 0;
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			aquecimentos = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			nucleo = atoi(argv[++i]);
		} else {
			uso(argv[0]);
			return 2;
		}
	}
	if (n < 1 || n > 100000 || aquecimentos < 0 || nucleo < 0) {
		uso(argv[0]);
		return 2;
	}

	uint64_t f_timer = ler_cntfrq();
	double ns_por_tick = f_timer ? 1e9 / (double)f_timer : 0.0;

	/* ---- isolamento, antes de qualquer medicao ---- */
	bool ok_afinidade = prender_no_nucleo(nucleo);
	int  erro_afinidade = ok_afinidade ? 0 : errno;

	bool ok_fifo = virar_tempo_real(80);
	int  erro_fifo = ok_fifo ? 0 : errno;

	bool ok_mlock = fixar_memoria();
	int  erro_mlock = ok_mlock ? 0 : errno;

	char governor[64];
	ler_governor(nucleo, governor, sizeof(governor));

	uint64_t f_antes = freq_nucleo_hz(nucleo);

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

	uint64_t f_depois = freq_nucleo_hz(nucleo);
	int nucleo_real = sched_getcpu();

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

	/* ================================================================ */
	/* AS DUAS CONVERSOES                                               */
	/* ================================================================ */
	/*
	 * A primeira e' exata. f_timer e' constante, gravada pelo firmware, e
	 * nao depende de nada que o sistema faca.
	 */
	double us_por_tick = f_timer ? 1e6 / (double)f_timer : 0.0;

	/*
	 * A segunda e' derivada, e so' vale sob duas condicoes: conhecer
	 * f_nucleo, e ele nao ter mudado durante o lote. Quando qualquer uma das
	 * duas falha, a coluna sai VAZIA no CSV. Vazio e' diferente de zero:
	 * "nao medi" nao e' "custou zero ciclos", e um campo em branco impede que
	 * alguem, meses depois, some uma coluna achando que o dado esta' la'.
	 */
	bool clock_estavel = (f_antes != 0) && (f_antes == f_depois);
	double ciclos_por_tick = clock_estavel
		? (double)f_antes / (double)f_timer : 0.0;

	double res_rel = mediana > 0 ? 100.0 / mediana : 0.0;
	double dispersao = mediana > 0
		? 100.0 * (double)(maximo - minimo) / mediana : 0.0;
	bool isolamento_completo = ok_afinidade && ok_fifo && ok_mlock &&
		strcmp(governor, "performance") == 0;
	bool utilizavel = clock_estavel && isolamento_completo && mediana >= 100.0;

	/* ================================================================ */
	/* CANAL 1: stdout, uma linha de CSV                                */
	/* ================================================================ */

	printf("%s,%d,%d,%d,%s,"
	       "%llu,%llu,%d,%d,"
	       "%llu,%.1f,%.1f,%llu,"
	       "%.4f,%.4f,",
	       BENCH_NAME, n, aquecimentos, nucleo_real, governor,
	       (unsigned long long)f_antes, (unsigned long long)f_timer,
	       clock_estavel ? 1 : 0, isolamento_completo ? 1 : 0,
	       (unsigned long long)minimo, mediana, media,
	       (unsigned long long)maximo,
	       media * us_por_tick, maximo * us_por_tick);

	if (clock_estavel)
		printf("%.0f,%.0f,%.0f,",
		       media * ciclos_por_tick,
		       mediana * ciclos_por_tick,
		       maximo * ciclos_por_tick);
	else
		printf(",,,");        /* vazio, e nao zero */

	printf("%.2f,%.1f,%d\n", res_rel, dispersao, utilizavel ? 1 : 0);
	fflush(stdout);

	/* ================================================================ */
	/* CANAL 2: stderr, o relatorio legivel                             */
	/* ================================================================ */

	fprintf(stderr, "etapa7 — benchmark \"%s\"\n", BENCH_NAME);
	fprintf(stderr, "  %d execucoes medidas, %d de aquecimento\n",
		n, aquecimentos);
	fprintf(stderr, "  CNTFRQ_EL0 = %.3f MHz (%.2f ns/tick)\n\n",
		f_timer / 1e6, ns_por_tick);

	fprintf(stderr, "AMBIENTE\n");
	fprintf(stderr, "  afinidade   nucleo %d          %s\n", nucleo,
		ok_afinidade ? "OK" : strerror(erro_afinidade));
	fprintf(stderr, "  nucleo real onde rodou: %d      %s\n", nucleo_real,
		nucleo_real == nucleo ? "" : "<<< NAO e' o pedido");
	fprintf(stderr, "  SCHED_FIFO  prioridade 80     %s\n",
		ok_fifo ? "OK" : strerror(erro_fifo));
	fprintf(stderr, "  mlockall    CURRENT|FUTURE    %s\n",
		ok_mlock ? "OK" : strerror(erro_mlock));
	fprintf(stderr, "  governor    %-16s %s\n", governor,
		strcmp(governor, "performance") == 0 ? "OK" : "<<< nao e' performance");
	fprintf(stderr, "  clock       %.0f MHz antes, %.0f MHz depois   %s\n",
		f_antes / 1e6, f_depois / 1e6,
		clock_estavel ? "estavel" : "<<< MUDOU");

	if (!ok_fifo || !ok_mlock)
		fprintf(stderr,
			"\n  >>> Isolamento PARCIAL. Rode com sudo para o lote completo.\n");
	if (strcmp(governor, "performance") != 0)
		fprintf(stderr,
			"\n  >>> Para travar o clock:\n"
			"      echo performance | sudo tee "
			"/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "AMOSTRAS\n");
	for (int i = 0; i < n && i < 20; i++)
		fprintf(stderr, "  execucao %2d   %10llu ticks   %10.4f us\n",
			i + 1, (unsigned long long)amostras[i],
			amostras[i] * us_por_tick);
	if (n > 20)
		fprintf(stderr, "  ... (%d amostras restantes omitidas)\n", n - 20);
	fprintf(stderr, "\n");

	/* As tres colunas lado a lado, de proposito. A conversao fica auditavel:
	 * quem ler o relatorio ve' o dado bruto, o dado exato e o dado derivado, e
	 * pode refazer a conta. Reportar so' os ciclos esconderia que eles nao
	 * foram medidos, e sim calculados a partir de uma frequencia lida do
	 * sysfs. */
	fprintf(stderr, "RESUMO           ticks          us       ciclos (derivados)\n");
	fprintf(stderr, "  minimo   %10llu  %10.4f  %s\n",
		(unsigned long long)minimo, minimo * us_por_tick,
		clock_estavel ? "" : "(indisponivel)");
	fprintf(stderr, "  mediana  %10.1f  %10.4f  ", mediana,
		mediana * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f\n", mediana * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "  MEDIA    %10.1f  %10.4f  ", media, media * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f   <- \"WCET observado\" do paper\n",
			media * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "  maximo   %10llu  %10.4f  ",
		(unsigned long long)maximo, maximo * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f   <- proxy de pior caso\n",
			maximo * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "CONVERSAO\n");
	fprintf(stderr, "  ticks -> us       exata:    /%.3f MHz\n", f_timer / 1e6);
	if (clock_estavel)
		fprintf(stderr,
			"  ticks -> ciclos   DERIVADA: x %.4f  (= %.0f MHz / %.0f MHz)\n"
			"                    supoe clock parado no lote inteiro; conferido\n"
			"                    lendo scaling_cur_freq antes e depois.\n",
			ciclos_por_tick, f_antes / 1e6, f_timer / 1e6);
	else
		fprintf(stderr,
			"  ticks -> ciclos   RECUSADA: o clock do nucleo mudou durante o\n"
			"                    lote (%.0f -> %.0f MHz). Converter aqui daria\n"
			"                    um numero sem significado.\n",
			f_antes / 1e6, f_depois / 1e6);
	fprintf(stderr, "\n");

	fprintf(stderr, "DIAGNOSTICO\n");
	fprintf(stderr, "  dispersao (max-min)/mediana   %6.1f %%\n", dispersao);
	fprintf(stderr, "  media/mediana                 %6.3f\n",
		mediana > 0 ? media / mediana : 0.0);
	fprintf(stderr, "  resolucao relativa (1 tick)   %6.2f %%\n", res_rel);

	if (mediana < 100.0)
		fprintf(stderr,
			"  AVISO: mediana de %.1f ticks. A regua de %.2f ns nao resolve "
			"este benchmark;\n"
			"         a dispersao acima e' majoritariamente quantizacao. "
			"Numero NAO utilizavel.\n", mediana, ns_por_tick);
	else if (mediana < 1000.0)
		fprintf(stderr,
			"  ATENCAO: mediana de %.1f ticks da' so' %.2f %% de resolucao. "
			"Use com ressalva.\n", mediana, res_rel);

	fprintf(stderr, "  utilizavel = %d  (clock estavel, isolamento completo, "
			"mediana >= 100 ticks)\n", utilizavel ? 1 : 0);

	delete[] ordenadas;
	delete[] amostras;
	return 0;
}
