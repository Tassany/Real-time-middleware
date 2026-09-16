/*
 * measure_wcet.cpp — porte para x86_64 do harness de WCET observado.
 *
 * Irmão de ../cycle_counter/measure_wcet.cpp (ver RELATORIO.md ao lado dele
 * para a metodologia completa, validada numa Raspberry Pi 5/ARMv8-A). Este
 * arquivo existe à parte, em vez de #ifdef dentro do original, para não
 * arriscar quebrar aquela ferramenta já validada.
 *
 * O QUE MUDA EM RELAÇÃO AO ORIGINAL, E POR QUÊ:
 *
 *   - O original tem DOIS caminhos para ciclos de núcleo: `mrs pmccntr_el0`
 *     direto (exige um módulo de kernel ARM que autoriza acesso a partir de
 *     EL0) e `perf_event_open()` como fallback sem módulo. Em x86 não existe
 *     essa distinção: perf_event_open() é a chamada de sistema genérica do
 *     Linux para o mesmo tipo de contador de hardware, em qualquer
 *     arquitetura que o kernel suporte, e é o único caminho aqui. Todo o
 *     aparato de capturar SIGILL para testar se o registrador está acessível
 *     (pmu_acessivel/pmu_contando no original) some — não há registrador de
 *     usuário para testar.
 *
 *   - O original lê `CNTVCT_EL0`, o generic timer do ARM: um contador físico
 *     SEPARADO do clock do núcleo, com frequência fixa gravada pelo firmware
 *     (`CNTFRQ_EL0`). Isso permite converter ticks em nanossegundos de forma
 *     EXATA, e em ciclos de forma DERIVADA (ticks × f_núcleo / f_timer),
 *     supondo que o clock não mudou durante o lote. x86 não expõe um
 *     equivalente de uso geral do mesmo jeito (o TSC é outra história, com
 *     outras armadilhas de invariância entre núcleos e não vale a pena
 *     reintroduzir aqui). Em vez disso este arquivo usa
 *     `clock_gettime(CLOCK_MONOTONIC)`, que já é a fonte de tempo que o
 *     resto do middleware usa (ver src/dispatcher.hpp) — mantém a medição
 *     de tempo e a de ciclos como DUAS colunas independentes, sem a
 *     conversão derivada nem a checagem de "clock mudou no meio do lote"
 *     que ela exigia no original.
 *
 *   - A resolução de `clock_gettime` é lida de `clock_getres()` em vez de
 *     assumida como "1 tick" fixo. Em x86 moderno isso costuma dar 1 ns,
 *     ordens de grandeza melhor que os 18,52 ns/tick do generic timer da
 *     Pi 5 — a preocupação do original com "régua grossa demais" para
 *     benchmarks curtos (seção 5.2 do RELATORIO.md) tende a não se repetir
 *     aqui, mas o código ainda calcula e reporta a métrica, para o caso de
 *     rodar num ambiente com relógio de resolução pior (ex.: uma VM).
 *
 * O QUE FICA IGUAL, porque é POSIX/Linux genérico ou é sobre as FONTES
 * Mälardalen e não sobre a arquitetura do processador:
 *
 *   - Isolamento: sched_setaffinity (prender a um núcleo), SCHED_FIFO
 *     prioridade 80, mlockall(MCL_CURRENT|MCL_FUTURE).
 *   - Leitura de governor/frequência via sysfs
 *     (/sys/devices/system/cpu/cpuN/cpufreq/...) — mesmos caminhos em x86,
 *     desde que a máquina não esteja em intel_pstate modo ativo sem
 *     governors clássicos (checar `scaling_driver` antes de medir de
 *     verdade).
 *   - Particularidades por benchmark: bsort100 lê um endereço absoluto
 *     (mapeado via mmap), recursion precisa de um sumidouro externo `In`,
 *     crc/fft1 têm estado estático que o aquecimento pode mascarar.
 *   - Renomear main() via `-Dmain=bench_entry` e ligação por `extern "C"`.
 *   - `perf_fd` como alvo de corrupção: select.c e qsort-exam.c escrevem
 *     fora dos limites de um vetor global e acertam esse descritor quando
 *     ligados junto — por isso ficam de fora da lista BENCHES do Makefile,
 *     igual ao original, e por isso perf_ler() continua nunca devolvendo 0
 *     em silêncio quando o read() falha.
 *
 * Uso:  ./bin_wcet/<nome> [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <algorithm>

#include <sched.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/perf_event.h>

#ifndef BENCH_NAME
#define BENCH_NAME "desconhecido"
#endif

/* ================================================================== */
/* Ligação com os benchmarks Malardalen — ver comentário longo no      */
/* original (../cycle_counter/measure_wcet.cpp) sobre -Dmain=bench_entry, */
/* extern "C" e o porquê de void em vez de int como tipo de retorno.    */
/* ================================================================== */

extern "C" void bench_entry();

/* ================================================================== */
/* bsort100: endereço absoluto cravado na fonte — ver comentário longo  */
/* no original. Fonte intocada; o harness mapeia a página que falta.   */
/* ================================================================== */
#ifdef MAPEAR_ENDERECO_FIXO

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

/* ================================================================== */
/* recursion: sumidouro externo que a fonte declara e nunca define.    */
/* Ver comentario longo no original.                                   */
/* ================================================================== */
#ifdef DEFINIR_IN
extern "C" {
volatile int In = 0;
}
#endif

/* ------------------------------------------- instrumento de tempo ---- */

static inline uint64_t ler_ns()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void barreira()
{
	asm volatile("" : : : "memory");
}

/* ================================================================== */
/* CICLOS DE NUCLEO via perf_event_open — unico caminho aqui           */
/* ================================================================== */
/*
 * Ver a explicacao completa no original: PERF_TYPE_HARDWARE +
 * PERF_COUNT_HW_CPU_CYCLES pede o mesmo tipo de contador de hardware que o
 * `perf` usa, `exclude_kernel=1` faz a PMU parar de contar enquanto a CPU
 * esta' em modo kernel (o que tira o custo do proprio read() da contagem, e
 * separa custo do PROGRAMA de custo do SISTEMA OPERACIONAL), e `pinned=1`
 * pede que o contador fique no hardware o lote inteiro. `pid=0, cpu=-1`
 * quer dizer "esta thread, em qualquer nucleo" — como a thread ja' esta'
 * presa por sched_setaffinity, o contador segue o nucleo medido.
 */

static int perf_fd = -1;

static bool perf_abrir()
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.type           = PERF_TYPE_HARDWARE;
	attr.size           = sizeof(attr);
	attr.config         = PERF_COUNT_HW_CPU_CYCLES;
	attr.disabled       = 1;
	attr.pinned         = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv     = 1;

	long fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0UL);

	if (fd < 0)
		return false;

	perf_fd = (int)fd;

	if (ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
	    ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
		close(perf_fd);
		perf_fd = -1;
		return false;
	}
	return true;
}

/* Levantada quando uma leitura do contador falha no meio do lote, e nunca
 * abaixada. Ver o comentario de perf_ler() no original: um benchmark que
 * escreve fora dos limites de um vetor global pode corromper este
 * descritor, e a funcao precisa DIZER que quebrou em vez de devolver um
 * zero que parece medida. */
static bool perf_quebrou = false;
static int  perf_erro    = 0;

static inline uint64_t perf_ler()
{
	uint64_t v = 0;

	if (read(perf_fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
		if (!perf_quebrou) {
			perf_quebrou = true;
			perf_erro = errno;
		}
		return 0;
	}
	return v;
}

enum fonte_ciclos {
	CICLOS_NENHUM = 0,   /* perf_event_open indisponivel */
	CICLOS_PERF   = 1    /* perf_event_open, PERF_COUNT_HW_CPU_CYCLES */
};

static const char *nome_fonte(int f)
{
	return f == CICLOS_PERF ? "perf" : "nenhuma";
}

/* Piso do contador de ciclos: mesma forma da janela real (duas leituras de
 * tempo DENTRO da janela de ciclos), para que o piso já inclua o custo
 * delas, que é exatamente o que se quer descontar depois. */
static uint64_t medir_piso_pmu()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t c1 = perf_ler();

		(void)ler_ns();
		(void)ler_ns();

		uint64_t c2 = perf_ler();
		uint64_t d = c2 - c1;

		if (d < menor)
			menor = d;
	}
	return menor;
}

/* Piso do instrumento de tempo: duas leituras consecutivas de clock_gettime,
 * minimo de mil tentativas. Ver secao "PISO DO INSTRUMENTO" no original. */
static uint64_t medir_piso_ns()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t a = ler_ns();
		uint64_t b = ler_ns();

		if (b - a < menor)
			menor = b - a;
	}
	return menor;
}

/* ================================================================== */
/* ISOLAMENTO — identico ao original, POSIX/Linux generico             */
/* ================================================================== */

static bool prender_no_nucleo(int nucleo)
{
	cpu_set_t conjunto;

	CPU_ZERO(&conjunto);
	CPU_SET(nucleo, &conjunto);

	return sched_setaffinity(0, sizeof(conjunto), &conjunto) == 0;
}

static bool virar_tempo_real(int prioridade)
{
	struct sched_param p;

	memset(&p, 0, sizeof(p));
	p.sched_priority = prioridade;

	return sched_setscheduler(0, SCHED_FIFO, &p) == 0;
}

static bool fixar_memoria()
{
	return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

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

/* Frequencia atual do nucleo, em Hz. O sysfs reporta em kHz. Mesmo caminho
 * em x86, desde que a maquina nao esteja com intel_pstate em modo ativo sem
 * um governor classico — confira `scaling_driver` antes de medir de
 * verdade; se este arquivo nao existir a leitura devolve 0 e o programa
 * trata isso como "clock instavel/desconhecido", nunca como 0 Hz real. */
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
		"  --amostras        despeja as n amostras cruas, uma por linha, em\n"
		"                    stderr, sem o corte de 20 do relatorio\n"
		"\n"
		"stdout leva uma linha de CSV; o relatorio legivel vai para stderr.\n"
		"Rode com sudo para conseguir SCHED_FIFO e mlockall. Confira tambem\n"
		"/proc/sys/kernel/perf_event_paranoid (precisa ser <= 2, ou rode como\n"
		"root) para o perf_event_open funcionar.\n", prog);
}

static const char *CSV_CABECALHO =
	"benchmark,n,aquecimentos,nucleo,governor,"
	"f_nucleo_hz_antes,f_nucleo_hz_depois,clock_estavel,isolamento_completo,"
	"min_ns,mediana_ns,media_ns,max_ns,"
	"min_us,mediana_us,media_us,max_us,"
	"piso_ns,piso_pct_mediana,resolucao_rel_pct,dispersao_pct,"
	"incerteza_media_pct,max_confiavel,"
	"pmu_ok,min_ciclos,mediana_ciclos,media_ciclos,max_ciclos,"
	"piso_ciclos,mediana_ciclos_liq,resolucao_pmu_pct,dispersao_pmu_pct,"
	"fonte_ciclos";

int main(int argc, char **argv)
{
	int n = 5;
	int aquecimentos = 1;
	int nucleo = 3;
	bool despejar_amostras = false;

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
		} else if (!strcmp(argv[i], "--amostras")) {
			despejar_amostras = true;
		} else {
			uso(argv[0]);
			return 2;
		}
	}
	if (n < 1 || n > 100000 || aquecimentos < 0 || nucleo < 0) {
		uso(argv[0]);
		return 2;
	}

	struct timespec res;
	clock_getres(CLOCK_MONOTONIC, &res);
	uint64_t res_ns = std::max<uint64_t>(1,
		(uint64_t)res.tv_sec * 1000000000ULL + (uint64_t)res.tv_nsec);

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

	/* ---- fonte de ciclos ---- */
	int fonte = CICLOS_NENHUM;

	if (perf_abrir()) {
		fonte = CICLOS_PERF;
	} else {
		fprintf(stderr,
			"[%s] aviso: perf_event_open falhou (%s).\n"
			"      Verifique /proc/sys/kernel/perf_event_paranoid (precisa "
			"ser <= 2)\n"
			"      ou rode com sudo. Seguindo so' com clock_gettime.\n",
			BENCH_NAME, strerror(errno));
	}

	bool pmu_ok = (fonte != CICLOS_NENHUM);

	uint64_t piso = medir_piso_ns();
	uint64_t piso_pmu = pmu_ok ? medir_piso_pmu() : 0;

	uint64_t *amostras = new uint64_t[n];
	uint64_t *ciclos_medidos = new uint64_t[n];

	for (int i = 0; i < aquecimentos; i++) {
		bench_entry();
		barreira();
	}

	if (fonte == CICLOS_PERF) {
		/* A janela de ciclos envolve a de tempo, por fora: perf_ler() e' um
		 * read(), e uma chamada de sistema dentro da janela de tempo entraria
		 * inteira na contagem. Por fora ela nao entra, e o exclude_kernel
		 * garante que o tempo dela tambem nao entra na contagem de ciclos. */
		for (int i = 0; i < n; i++) {
			uint64_t c0 = perf_ler();
			uint64_t t0 = ler_ns();

			bench_entry();

			uint64_t t1 = ler_ns();
			uint64_t c1 = perf_ler();

			barreira();
			amostras[i] = t1 - t0;
			ciclos_medidos[i] = c1 - c0;
		}
	} else {
		for (int i = 0; i < n; i++) {
			uint64_t t0 = ler_ns();

			bench_entry();

			uint64_t t1 = ler_ns();

			barreira();
			amostras[i] = t1 - t0;
			ciclos_medidos[i] = 0;
		}
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

	if (perf_quebrou) {
		pmu_ok = false;
		fprintf(stderr,
			"\n[%s] >>> O CONTADOR DE CICLOS QUEBROU DURANTE O LOTE <<<\n"
			"      read(perf_fd) falhou: %s\n"
			"      As colunas de ciclos deste benchmark saem vazias. As de\n"
			"      tempo continuam validas. Saida com codigo 3.\n\n",
			BENCH_NAME, strerror(perf_erro));
	}

	uint64_t *ord_pmu = new uint64_t[n];
	double media_pmu = 0.0, mediana_pmu = 0.0;
	uint64_t min_pmu = 0, max_pmu = 0;

	if (pmu_ok) {
		std::copy(ciclos_medidos, ciclos_medidos + n, ord_pmu);
		std::sort(ord_pmu, ord_pmu + n);

		double soma_pmu = 0.0;
		for (int i = 0; i < n; i++)
			soma_pmu += (double)ord_pmu[i];

		media_pmu   = soma_pmu / n;
		min_pmu     = ord_pmu[0];
		max_pmu     = ord_pmu[n - 1];
		mediana_pmu = (n % 2)
			? (double)ord_pmu[n / 2]
			: ((double)ord_pmu[n / 2 - 1] + (double)ord_pmu[n / 2]) / 2.0;
	}

	bool clock_estavel = (f_antes != 0) && (f_antes == f_depois);
	bool isolamento_completo = ok_afinidade && ok_fifo && ok_mlock &&
		strcmp(governor, "performance") == 0;

	double res_rel = mediana > 0 ? 100.0 * (double)res_ns / mediana : 0.0;
	double dispersao = mediana > 0
		? 100.0 * (double)(maximo - minimo) / mediana : 0.0;
	double incerteza_media = mediana > 0
		? 100.0 * (double)res_ns / (mediana * sqrt(12.0 * n)) : 0.0;
	double piso_pct = mediana > 0 ? 100.0 * (double)piso / mediana : 0.0;

	/* max_confiavel generaliza o "mediana >= 100 ticks" do original: exige
	 * a mediana pelo menos ~20x a resolucao medida do relogio, para que um
	 * erro de arredondamento nao seja confundido com uma execucao lenta. */
	bool max_confiavel = clock_estavel && isolamento_completo &&
		mediana >= (double)(res_ns * 20);

	double medido_liq = mediana_pmu - (double)piso_pmu;

	/* ---- CANAL 1: stdout, uma linha de CSV ---- */
	printf("%s,%d,%d,%d,%s,"
	       "%llu,%llu,%d,%d,"
	       "%llu,%.1f,%.1f,%llu,"
	       "%.4f,%.4f,%.4f,%.4f,"
	       "%llu,%.1f,%.2f,%.1f,"
	       "%.3f,%d,",
	       BENCH_NAME, n, aquecimentos, nucleo_real, governor,
	       (unsigned long long)f_antes, (unsigned long long)f_depois,
	       clock_estavel ? 1 : 0, isolamento_completo ? 1 : 0,
	       (unsigned long long)minimo, mediana, media,
	       (unsigned long long)maximo,
	       minimo / 1000.0, mediana / 1000.0, media / 1000.0, maximo / 1000.0,
	       (unsigned long long)piso, piso_pct, res_rel, dispersao,
	       incerteza_media, max_confiavel ? 1 : 0);

	if (pmu_ok)
		printf("1,%llu,%.1f,%.1f,%llu,%llu,%.0f,%.4f,%.1f,%s\n",
		       (unsigned long long)min_pmu, mediana_pmu, media_pmu,
		       (unsigned long long)max_pmu, (unsigned long long)piso_pmu,
		       medido_liq,
		       mediana_pmu > 0 ? 100.0 / mediana_pmu : 0.0,
		       mediana_pmu > 0
			       ? 100.0 * (double)(max_pmu - min_pmu) / mediana_pmu : 0.0,
		       nome_fonte(fonte));
	else
		printf("0,,,,,,,,,%s\n", nome_fonte(fonte));   /* vazio, e nao zero */
	fflush(stdout);

	/* ---- CANAL 2: stderr, o relatorio legivel ---- */
	fprintf(stderr, "measure_wcet (x86) — benchmark \"%s\"\n", BENCH_NAME);
	fprintf(stderr, "  %d execucoes medidas, %d de aquecimento\n",
		n, aquecimentos);
	fprintf(stderr, "  resolucao de CLOCK_MONOTONIC: %llu ns\n\n",
		(unsigned long long)res_ns);

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
		clock_estavel ? "estavel" : "<<< MUDOU ou indisponivel via sysfs");
	fprintf(stderr, "  ciclos via  %-16s %s\n", nome_fonte(fonte),
		fonte == CICLOS_PERF ? "perf_event_open, exclude_kernel=1" :
				       "<<< INDISPONIVEL, so' clock_gettime");

	if (!ok_fifo || !ok_mlock)
		fprintf(stderr,
			"\n  >>> Isolamento PARCIAL. Rode com sudo para o lote completo.\n");
	if (strcmp(governor, "performance") != 0)
		fprintf(stderr,
			"\n  >>> Para travar o clock:\n"
			"      echo performance | sudo tee "
			"/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor\n");
	if (!pmu_ok)
		fprintf(stderr,
			"\n  >>> Sem perf_event_open, a coluna de ciclos fica vazia.\n"
			"      cat /proc/sys/kernel/perf_event_paranoid   (precisa <= 2)\n");
	fprintf(stderr, "\n");

	if (despejar_amostras) {
		fprintf(stderr, "# amostras de %s, na ordem em que foram medidas\n",
			BENCH_NAME);
		fprintf(stderr, "# execucao ns us ciclos_pmu\n");
		for (int i = 0; i < n; i++)
			fprintf(stderr, "%d %llu %.4f %llu\n",
				i + 1,
				(unsigned long long)amostras[i],
				amostras[i] / 1000.0,
				(unsigned long long)(pmu_ok ? ciclos_medidos[i] : 0));
	} else {
		fprintf(stderr, "AMOSTRAS\n");
		for (int i = 0; i < n && i < 20; i++)
			fprintf(stderr, "  execucao %2d   %10llu ns   %10.4f us\n",
				i + 1, (unsigned long long)amostras[i],
				amostras[i] / 1000.0);
		if (n > 20)
			fprintf(stderr, "  ... (%d amostras restantes omitidas)\n",
				n - 20);
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "RESUMO (tempo, via CLOCK_MONOTONIC)   ns          us\n");
	fprintf(stderr, "  minimo   %14llu  %10.4f\n",
		(unsigned long long)minimo, minimo / 1000.0);
	fprintf(stderr, "  mediana  %14.1f  %10.4f\n", mediana, mediana / 1000.0);
	fprintf(stderr, "  MEDIA    %14.1f  %10.4f   <- \"WCET observado\" (tempo)\n",
		media, media / 1000.0);
	fprintf(stderr, "  maximo   %14llu  %10.4f   <- proxy de pior caso (tempo)\n",
		(unsigned long long)maximo, maximo / 1000.0);
	fprintf(stderr, "\n");

	if (pmu_ok) {
		fprintf(stderr, "CICLOS DE NUCLEO (via %s, exclude_kernel=1)\n",
			nome_fonte(fonte));
		fprintf(stderr, "  minimo   %12llu\n", (unsigned long long)min_pmu);
		fprintf(stderr, "  mediana  %12.1f\n", mediana_pmu);
		fprintf(stderr, "  MEDIA    %12.1f   <- \"WCET observado\" (ciclos)\n",
			media_pmu);
		fprintf(stderr, "  maximo   %12llu   <- proxy de pior caso (ciclos)\n",
			(unsigned long long)max_pmu);
		fprintf(stderr, "  piso do instrumento  %llu ciclos = %.2f %% da mediana\n",
			(unsigned long long)piso_pmu,
			mediana_pmu > 0 ? 100.0 * (double)piso_pmu / mediana_pmu : 0.0);
		fprintf(stderr,
			"  DESCONTADO O PISO: %.0f ciclos liquidos (%.1f - %llu)\n",
			medido_liq, mediana_pmu, (unsigned long long)piso_pmu);
		fprintf(stderr, "\n");
	}

	fprintf(stderr, "DIAGNOSTICO\n");
	fprintf(stderr, "  dispersao (max-min)/mediana   %6.1f %%\n", dispersao);
	fprintf(stderr, "  media/mediana                 %6.3f\n",
		mediana > 0 ? media / mediana : 0.0);
	fprintf(stderr, "  resolucao relativa (%llu ns)   %6.4f %%  em UMA amostra\n",
		(unsigned long long)res_ns, res_rel);
	fprintf(stderr, "  incerteza da media (n=%d)     %6.4f %%  a quantizacao "
			"se cancela na media\n", n, incerteza_media);
	fprintf(stderr, "  piso do instrumento de tempo   %6llu ns = %.2f %% da "
			"mediana\n", (unsigned long long)piso, piso_pct);
	fprintf(stderr, "\n");

	if (res_rel > 5.0)
		fprintf(stderr,
			"  RESOLUCAO BAIXA (%llu ns = %.2f %% da mediana). Isso e' "
			"incomum em\n"
			"    x86 nativo (esperado ~1ns); se estiver rodando numa VM, "
			"o relogio\n"
			"    do hipervisor pode ser mais grosseiro. MEDIA ainda e' "
			"confiavel\n"
			"    (arredondamento se cancela em %d amostras); MAXIMO "
			"nao e'.\n",
			(unsigned long long)res_ns, res_rel, n);

	if (piso_pct > 5.0)
		fprintf(stderr,
			"  VIES SISTEMATICO: o piso do instrumento de tempo e' %.1f %% "
			"da mediana.\n"
			"    Esse custo esta' DENTRO de todas as amostras. O programa "
			"NAO subtrai,\n"
			"    porque supor sobreposicao nula entre a leitura e o "
			"benchmark seria\n"
			"    errado num nucleo fora de ordem. Trate o valor reportado "
			"como um\n"
			"    limite superior do custo real.\n",
			piso_pct);

	fprintf(stderr, "\n  max_confiavel = %d  (clock estavel, isolamento "
			"completo, mediana >= 20x a resolucao do relogio)\n",
			max_confiavel ? 1 : 0);

	if (nucleo_real != nucleo)
		fprintf(stderr,
			"\n  aviso: a thread terminou no nucleo %d e nao no %d "
			"pedido.\n"
			"    O perf acompanha a thread (nao o nucleo), entao a "
			"contagem de\n"
			"    ciclos continua valida, mas a migracao esfriou cache e "
			"preditor\n"
			"    e provavelmente inflou algumas amostras de tempo.\n",
			nucleo_real, nucleo);

	if (perf_fd >= 0)
		close(perf_fd);

	delete[] ord_pmu;
	delete[] ciclos_medidos;
	delete[] ordenadas;
	delete[] amostras;

	/* 3 e nao 0, para que um lote inteiro rodando dentro de um `for` do shell
	 * deixe rastro em vez de passar despercebido. */
	return perf_quebrou ? 3 : 0;
}
