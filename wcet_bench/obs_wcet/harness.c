/*
 * harness.c — observed-WCET measurement harness for ARMv8-A (Raspberry Pi 5).
 *
 * Reproduces the empirical protocol of Li et al., "WCET Analysis Based on
 * Micro-Architecture Modeling for Embedded System Security", Appl. Sci. 14
 * (2024) 7277, Section 5:
 *
 *   "Execution time in processor cycles is obtained by reading the generic
 *    timer register of the ARMv8-A CPU. The difference between the values
 *    before and after execution is the measured execution time of the program.
 *    Five measured execution times are averaged to obtain the final observed
 *    WCET."
 *
 * Two clarifications the paper glosses over, both made explicit here:
 *
 *   - The generic timer does NOT count CPU cycles.  It ticks at CNTFRQ_EL0
 *     (54 MHz on the Pi, ~18.52 ns per tick) regardless of the core's clock.
 *     Cycles are therefore a derived quantity, ticks * f_cpu / f_timer, and we
 *     report ticks, microseconds and converted cycles in separate columns so
 *     the conversion stays auditable.  Since the paper's figures are plotted in
 *     processor cycles, we additionally count real cycles with the PMU via
 *     perf_event_open, so the cycle comparison rests on a measurement rather
 *     than on the assumption that the clock never moved.
 *
 *   - Averaging runs estimates the AVERAGE case, not the worst case.  We report
 *     the mean to match the paper, and the max alongside it as the honest
 *     worst-case proxy.
 *
 * One binary is built per benchmark: this file supplies main(), the benchmark's
 * own main() is renamed to bench_entry() by -Dmain=bench_entry at compile time.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#if !defined(__aarch64__)
#error "This harness reads CNTVCT_EL0/CNTFRQ_EL0 and only builds for AArch64. \
Build it on the Raspberry Pi 5 itself (or with an aarch64 cross-compiler); it \
cannot run on an x86_64 host."
#endif

#ifndef BENCH_NAME
#define BENCH_NAME "unknown"
#endif

/* The benchmark's renamed entry point.  Deliberately declared without a
 * prototype: matmult, fft1 and expint define "void main()" while the others
 * return int.  Across translation units that difference is harmless under the
 * AArch64 procedure call standard -- only x0 differs, and we discard it. */
extern int bench_entry();

#define DEFAULT_RUNS      20
#define DEFAULT_WARMUPS    1
#define MAX_RUNS      100000
/* Below this many ticks the measurement is within a couple of timer periods
 * and the trailing digits are quantisation noise rather than signal. */
#define LOW_RES_TICKS     50
/* At or below this the sample is indistinguishable from an empty timed region,
 * which on an optimised build means the benchmark was optimised away. */
#define DCE_SUSPECT_TICKS  3
/* Cache-eviction working set for cold mode. The Pi 5 (Cortex-A76) carries
 * 64 KiB L1D and 512 KiB L2 per core plus 2 MiB of shared L3; 8 MiB streams
 * past all three. */
#define FLUSH_BYTES  (8u << 20)
#define CACHE_LINE   64

/* ------------------------------------------------------------------ timer */

static inline uint64_t rd_cntvct(void)
{
	uint64_t v;
	/* isb before the read stops the counter access from being reordered
	 * ahead of the surrounding code; without it the measured window can
	 * drift by tens of cycles on an out-of-order core like the A76. */
	__asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}

static inline uint64_t rd_cntfrq(void)
{
	uint64_t v;
	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* -------------------------------------------------------------------- PMU */
/*
 * Real cycle counting, alongside the generic timer.
 *
 * The paper plots processor cycles, but its stated instrument -- the generic
 * timer -- cannot produce them: CNTVCT_EL0 runs at a fixed 54 MHz on the Pi and
 * knows nothing about the core clock.  Converting ticks to cycles therefore
 * assumes a clock that held steady for the whole batch.  PERF_COUNT_HW_CPU_CYCLES
 * counts the real thing, so the two columns can be checked against each other
 * instead of one being taken on faith.
 *
 * exclude_kernel means the PMU stops counting inside interrupt handlers while
 * the generic timer keeps running through them.  A sample where derived and
 * measured cycles diverge sharply is thus a sample the OS interrupted -- the
 * disagreement is a jitter detector, not a defect.
 *
 * Entirely optional.  If the event cannot be opened (perf_event_paranoid, no
 * PMU access under a hypervisor, an old kernel) we say so once and fall back to
 * exactly the behaviour that existed before: generic timer, derived cycles.
 */
struct pmu_read {
	uint64_t value;
	uint64_t time_enabled;
	uint64_t time_running;
};

static int pmu_fd = -1;

static int pmu_open(void)
{
	struct perf_event_attr attr;
	long fd;

	memset(&attr, 0, sizeof(attr));
	attr.type           = PERF_TYPE_HARDWARE;
	attr.size           = sizeof(attr);
	attr.config         = PERF_COUNT_HW_CPU_CYCLES;
	attr.disabled       = 1;
	/* pinned: keep the counter on hardware for the whole batch rather than
	 * letting it be multiplexed, which would scale the counts. */
	attr.pinned         = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv     = 1;
	attr.read_format    = PERF_FORMAT_TOTAL_TIME_ENABLED |
			      PERF_FORMAT_TOTAL_TIME_RUNNING;

	/* pid 0, cpu -1: this thread, wherever it is scheduled.  The thread is
	 * already pinned by taskset, so the counter follows the measured core. */
	fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0UL);
	if (fd < 0)
		return -1;
	pmu_fd = (int)fd;

	if (ioctl(pmu_fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
	    ioctl(pmu_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
		close(pmu_fd);
		pmu_fd = -1;
		return -1;
	}
	return 0;
}

/* Read the free-running counter.  Called outside the CNTVCT window -- see the
 * measurement loop for why. */
static inline int pmu_sample(struct pmu_read *r)
{
	if (pmu_fd < 0)
		return -1;
	if (read(pmu_fd, r, sizeof(*r)) != (ssize_t)sizeof(*r))
		return -1;
	return 0;
}

/* -------------------------------------------------------- cache eviction */
/*
 * Cold-cache mode (-f).
 *
 * Without it the harness measures steady state: N back-to-back executions in
 * one process leave code, data and page tables resident in L1, which is a
 * throughput figure rather than a worst case.  Static WCET analysis -- the
 * Chronos-derived tool this experiment exists to compare against included --
 * assumes an empty cache at program entry, so warm numbers are not comparable
 * with an estimated bound at all.
 *
 * EL0 cannot issue DC CISW, so we evict by construction instead:
 *   - data: stream a buffer larger than L1+L2+L3, reading it and then dirtying
 *     it, which forces write-back of everything the benchmark left behind;
 *   - instructions: __builtin___clear_cache() over our own text segment, which
 *     on AArch64 emits DC CVAU + IC IVAU -- both permitted at EL0 because Linux
 *     sets SCTLR_EL1.UCI.
 *
 * Warm-ups still run before the measured batch even in cold mode.  They pre-
 * fault the benchmark's .bss and grow the stack, so page-fault cost stays out
 * of the samples: the paper's reference tool models caches, and explicitly does
 * not model TLBs or demand paging.
 *
 * Out of reach from userspace, and therefore still warm: the branch predictor
 * and the prefetcher history.  Cold-mode numbers are a lower bound on a true
 * cold start, not an upper one.
 */
static unsigned char *flush_buf;
static volatile unsigned long flush_sink;
static uintptr_t text_lo, text_hi;

/* Locate our own executable mapping, so the I-cache invalidation covers the
 * benchmark code and everything it calls rather than a guessed range. */
static void find_text_range(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	uintptr_t target = (uintptr_t)(void *)&bench_entry;
	char line[512];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		char perms[8];

		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3)
			continue;
		if (perms[2] == 'x' && target >= lo && target < hi) {
			text_lo = lo;
			text_hi = hi;
			break;
		}
	}
	fclose(f);
}

static int flush_init(void)
{
	flush_buf = malloc(FLUSH_BYTES);
	if (!flush_buf)
		return -1;
	memset(flush_buf, 1, FLUSH_BYTES);
	find_text_range();
	return 0;
}

static void cache_flush(void)
{
	volatile unsigned char *p = flush_buf;
	unsigned long acc = 0;
	size_t i;

	/* Read pass evicts; write pass leaves the lines dirty so the next
	 * eviction cycle cannot be served from a clean copy. */
	for (i = 0; i < FLUSH_BYTES; i += CACHE_LINE)
		acc += p[i];
	for (i = 0; i < FLUSH_BYTES; i += CACHE_LINE)
		p[i] = (unsigned char)(acc + i);
	flush_sink = acc;

	if (text_hi > text_lo)
		__builtin___clear_cache((char *)text_lo, (char *)text_hi);
}

/* ------------------------------------------------------------------ sysfs */

static uint64_t read_sysfs_u64(const char *path)
{
	FILE *f = fopen(path, "r");
	unsigned long long v;

	if (!f)
		return 0;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	return (uint64_t)v;
}

/* Current clock of the given core, in Hz.  scaling_cur_freq is what the
 * governor has actually selected; cpuinfo_max_freq is the fallback when
 * cpufreq is not exposed. */
static uint64_t cpu_freq_hz(int cpu)
{
	char path[128];
	uint64_t khz;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
	khz = read_sysfs_u64(path);
	if (khz == 0) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		khz = read_sysfs_u64(path);
	}
	return khz * 1000ULL;
}

/* ------------------------------------------------------------- statistics */

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

struct stats {
	uint64_t min, max;
	double   mean, median;
};

static void compute_stats(const uint64_t *samples, int n, struct stats *s)
{
	uint64_t *sorted = malloc((size_t)n * sizeof(*sorted));
	double sum = 0.0;
	int i;

	if (!sorted) {
		fprintf(stderr, "harness: out of memory\n");
		exit(1);
	}
	memcpy(sorted, samples, (size_t)n * sizeof(*sorted));
	qsort(sorted, (size_t)n, sizeof(*sorted), cmp_u64);

	for (i = 0; i < n; i++)
		sum += (double)sorted[i];

	s->min    = sorted[0];
	s->max    = sorted[n - 1];
	s->mean   = sum / n;
	s->median = (n % 2) ? (double)sorted[n / 2]
			    : ((double)sorted[n / 2 - 1] + (double)sorted[n / 2]) / 2.0;
	free(sorted);
}

/* --------------------------------------------------------------- reporting */

static const char *CSV_HEADER =
	"benchmark,n,"
	"min_ticks,mean_ticks,median_ticks,max_ticks,"
	"min_us,mean_us,median_us,max_us,"
	"min_cycles,mean_cycles,median_cycles,max_cycles,"
	"cpu_freq_hz,timer_freq_hz,"
	/* Appended, never inserted: existing column positions stay put so the
	 * awk report in run_all.sh and any older analysis keep working. */
	"min_pmucyc,mean_pmucyc,median_pmucyc,max_pmucyc,pmu_ok";

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-n RUNS] [-w WARMUPS] [-r RAWDIR] [-f] [--header]\n"
		"\n"
		"  -n RUNS      measured executions (default %d; the paper uses 5)\n"
		"  -w WARMUPS   discarded executions before measuring (default %d)\n"
		"  -r RAWDIR    write per-run samples to RAWDIR/raw_<bench>.csv\n"
		"  -f           cold cache: evict L1/L2/L3 and invalidate the I-cache\n"
		"               before every measured run. Without it the numbers are\n"
		"               steady-state throughput, not a worst-case proxy.\n"
		"  --header     print the CSV header line and exit\n"
		"\n"
		"stdout carries one machine-readable CSV row; the human-readable\n"
		"report and all warnings go to stderr.\n",
		argv0, DEFAULT_RUNS, DEFAULT_WARMUPS);
}

int main(int argc, char **argv)
{
	int n = DEFAULT_RUNS, warmups = DEFAULT_WARMUPS, cold = 0;
	const char *rawdir = NULL;
	uint64_t *ticks, *pmucyc = NULL;
	uint64_t f_timer, f_cpu_before, f_cpu_after, f_cpu;
	uint64_t overhead = UINT64_MAX, pmu_overhead = UINT64_MAX;
	struct pmu_read pmu_first, pmu_last;
	int pmu_ok = 0;
	struct stats st, pst;
	double us_per_tick, cyc_per_tick;
	volatile int sink = 0;
	int cpu, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--header")) {
			puts(CSV_HEADER);
			return 0;
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			warmups = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			rawdir = argv[++i];
		} else if (!strcmp(argv[i], "-f")) {
			cold = 1;
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (n < 1 || n > MAX_RUNS || warmups < 0) {
		fprintf(stderr, "harness: RUNS must be in 1..%d, WARMUPS >= 0\n", MAX_RUNS);
		return 2;
	}

	f_timer = rd_cntfrq();
	if (f_timer == 0) {
		fprintf(stderr, "harness: CNTFRQ_EL0 reads 0; the firmware did not "
				"program the generic timer frequency\n");
		return 1;
	}

	/* Page faults taken mid-measurement would show up as execution time. */
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fprintf(stderr, "[%s] warning: mlockall failed (%s); page faults may "
				"perturb samples\n", BENCH_NAME, strerror(errno));

	cpu = sched_getcpu();
	f_cpu_before = cpu_freq_hz(cpu);

	if (cold && flush_init() != 0) {
		fprintf(stderr, "[%s] warning: cannot allocate the %u MiB eviction "
				"buffer; falling back to warm cache\n",
			BENCH_NAME, FLUSH_BYTES >> 20);
		cold = 0;
	}
	if (cold && text_hi == text_lo)
		fprintf(stderr, "[%s] warning: could not locate our text segment in "
				"/proc/self/maps; evicting data only, I-cache stays warm\n",
			BENCH_NAME);

	if (pmu_open() != 0)
		fprintf(stderr,
			"[%s] warning: perf_event_open(CPU_CYCLES) failed (%s); cycle "
			"columns will be derived from the generic timer only. Check "
			"/proc/sys/kernel/perf_event_paranoid or run as root.\n",
			BENCH_NAME, strerror(errno));
	else
		pmu_ok = 1;

	/* Cost of the instrumentation itself, so it can be judged against the
	 * shorter benchmarks rather than assumed negligible. */
	for (i = 0; i < 1000; i++) {
		uint64_t a = rd_cntvct();
		uint64_t b = rd_cntvct();

		if (b - a < overhead)
			overhead = b - a;
	}
	/* The same empty window as the measurement loop below, so this is exactly
	 * the amount by which the cycle window exceeds the tick window. */
	if (pmu_ok) {
		for (i = 0; i < 1000; i++) {
			struct pmu_read a, b;

			if (pmu_sample(&a) != 0)
				break;
			(void)rd_cntvct();
			(void)rd_cntvct();
			if (pmu_sample(&b) != 0)
				break;
			if (b.value - a.value < pmu_overhead)
				pmu_overhead = b.value - a.value;
		}
	}

	ticks = malloc((size_t)n * sizeof(*ticks));
	if (!ticks) {
		fprintf(stderr, "harness: out of memory\n");
		return 1;
	}
	if (pmu_ok) {
		pmucyc = malloc((size_t)n * sizeof(*pmucyc));
		if (!pmucyc) {
			fprintf(stderr, "harness: out of memory\n");
			return 1;
		}
	}

	for (i = 0; i < warmups; i++)
		sink = bench_entry();

	memset(&pmu_first, 0, sizeof(pmu_first));
	memset(&pmu_last, 0, sizeof(pmu_last));

	for (i = 0; i < n; i++) {
		uint64_t t0, t1;
		struct pmu_read c0, c1;

		if (cold)
			cache_flush();

		/* read() is a syscall and must not land inside the tick window, so
		 * the cycle window is nested outside it. The consequence is that
		 * cycle counts include the two rd_cntvct() reads while tick counts
		 * are untouched by the PMU reads; pmu_overhead above quantifies the
		 * difference. Sampling the counter after the flush keeps eviction
		 * cycles out of the measurement. */
		if (pmu_ok && pmu_sample(&c0) != 0) {
			fprintf(stderr, "[%s] warning: PMU read failed mid-batch (%s); "
					"dropping measured-cycle columns\n",
				BENCH_NAME, strerror(errno));
			pmu_ok = 0;
		}
		t0 = rd_cntvct();
		sink = bench_entry();
		t1 = rd_cntvct();
		if (pmu_ok && pmu_sample(&c1) != 0) {
			fprintf(stderr, "[%s] warning: PMU read failed mid-batch (%s); "
					"dropping measured-cycle columns\n",
				BENCH_NAME, strerror(errno));
			pmu_ok = 0;
		}

		ticks[i] = t1 - t0;
		if (pmu_ok) {
			pmucyc[i] = c1.value - c0.value;
			if (i == 0)
				pmu_first = c0;
			pmu_last = c1;
		}
	}
	(void)sink;

	/* pinned=1 should prevent multiplexing outright, but if the counter was
	 * ever descheduled the raw values are an undercount rather than something
	 * to silently scale. Say so and let the run be judged. */
	if (pmu_ok && pmu_last.time_enabled != pmu_last.time_running)
		fprintf(stderr,
			"[%s] warning: PMU event was multiplexed (enabled %llu ns vs "
			"running %llu ns); measured cycles undercount\n",
			BENCH_NAME,
			(unsigned long long)(pmu_last.time_enabled - pmu_first.time_enabled),
			(unsigned long long)(pmu_last.time_running - pmu_first.time_running));

	f_cpu_after = cpu_freq_hz(cpu);
	f_cpu = f_cpu_before ? f_cpu_before : f_cpu_after;

	compute_stats(ticks, n, &st);
	if (pmu_ok)
		compute_stats(pmucyc, n, &pst);

	us_per_tick  = 1e6 / (double)f_timer;
	cyc_per_tick = f_cpu ? (double)f_cpu / (double)f_timer : 0.0;

	/* ---- machine-readable row (stdout) ---- */
	printf("%s,%d,"
	       "%llu,%.2f,%.2f,%llu,"
	       "%.4f,%.4f,%.4f,%.4f,"
	       "%.1f,%.1f,%.1f,%.1f,"
	       "%llu,%llu",
	       BENCH_NAME, n,
	       (unsigned long long)st.min, st.mean, st.median, (unsigned long long)st.max,
	       st.min * us_per_tick, st.mean * us_per_tick,
	       st.median * us_per_tick, st.max * us_per_tick,
	       st.min * cyc_per_tick, st.mean * cyc_per_tick,
	       st.median * cyc_per_tick, st.max * cyc_per_tick,
	       (unsigned long long)f_cpu, (unsigned long long)f_timer);
	if (pmu_ok)
		printf(",%llu,%.1f,%.1f,%llu,1\n",
		       (unsigned long long)pst.min, pst.mean, pst.median,
		       (unsigned long long)pst.max);
	else
		printf(",,,,,0\n");   /* empty, not zero: absent is not "took 0 cycles" */
	fflush(stdout);

	/* ---- human-readable report (stderr) ---- */
	fprintf(stderr,
		"[%s] n=%d warmups=%d cache=%s core=%d f_cpu=%.3f GHz f_timer=%.3f MHz "
		"(%.3f ns/tick, read overhead %llu ticks)\n",
		BENCH_NAME, n, warmups, cold ? "COLD" : "warm", cpu,
		f_cpu / 1e9, f_timer / 1e6, 1e9 / (double)f_timer,
		(unsigned long long)overhead);
	fprintf(stderr,
		"[%s]   min %10.3f us | mean %10.3f us | median %10.3f us | MAX %10.3f us"
		"   (max/mean %.3f)\n",
		BENCH_NAME,
		st.min * us_per_tick, st.mean * us_per_tick,
		st.median * us_per_tick, st.max * us_per_tick,
		st.mean > 0.0 ? st.max / st.mean : 0.0);
	if (pmu_ok) {
		fprintf(stderr,
			"[%s]   PMU cycles: min %10llu | mean %12.1f | median %12.1f | "
			"MAX %10llu   (window overhead %llu cyc)\n",
			BENCH_NAME, (unsigned long long)pst.min, pst.mean, pst.median,
			(unsigned long long)pst.max,
			pmu_overhead == UINT64_MAX ? 0ULL
						   : (unsigned long long)pmu_overhead);
		/* Derived and measured cycles should agree to a few percent on a
		 * quiet, clock-locked core. They will not if the clock moved or an
		 * interrupt landed inside the window, which is the point of showing
		 * both rather than picking one. */
		if (cyc_per_tick > 0.0 && pst.mean > 0.0) {
			double derived = st.mean * cyc_per_tick;
			double skew = derived / pst.mean;

			if (skew < 0.9 || skew > 1.1)
				fprintf(stderr,
					"[%s] warning: derived cycles (%.0f) and measured "
					"cycles (%.0f) disagree by %.1f%%. The core clock "
					"likely moved, or the OS interrupted the timed "
					"window -- prefer the measured column.\n",
					BENCH_NAME, derived, pst.mean,
					(skew - 1.0) * 100.0);
		}
	}

	if (f_cpu_before && f_cpu_after && f_cpu_before != f_cpu_after)
		fprintf(stderr,
			"[%s] warning: core clock moved during the run (%llu -> %llu Hz); "
			"cycle columns use the pre-run value and are approximate\n",
			BENCH_NAME, (unsigned long long)f_cpu_before,
			(unsigned long long)f_cpu_after);
	if (f_cpu == 0)
		fprintf(stderr,
			"[%s] warning: could not read the core clock from sysfs; "
			"cycle columns are 0\n", BENCH_NAME);
	/* A couple of ticks is roughly the cost of the two counter reads alone.
	 * Optimised builds reach this when the compiler deletes the benchmark: the
	 * Malardalen sources discard their results, so at -O1 and above GCC can
	 * prove the work unobservable and drop it (fir writes only to a dead local;
	 * expint's return value is thrown away). That is a silent invalid
	 * measurement, not a fast one, so say so rather than just flagging it as
	 * short. */
	if (st.median <= DCE_SUSPECT_TICKS)
		fprintf(stderr,
			"[%s] WARNING: median is %.1f ticks, barely above the %llu-tick "
			"cost of reading the timer. If this was built with -O1 or higher, "
			"the compiler most likely eliminated the benchmark as dead code -- "
			"treat this row as invalid and rebuild with OPT=-O0.\n",
			BENCH_NAME, st.median, (unsigned long long)overhead);
	else if (st.median < LOW_RES_TICKS)
		fprintf(stderr,
			"[%s] warning: median is %.1f ticks, near the %.3f ns timer "
			"period -- this benchmark is too short to resolve reliably\n",
			BENCH_NAME, st.median, 1e9 / (double)f_timer);

	/* ---- per-run samples ---- */
	if (rawdir) {
		char path[512];
		FILE *f;

		snprintf(path, sizeof(path), "%s/raw_%s.csv", rawdir, BENCH_NAME);
		f = fopen(path, "w");
		if (!f) {
			fprintf(stderr, "[%s] warning: cannot write %s (%s)\n",
				BENCH_NAME, path, strerror(errno));
		} else {
			fprintf(f, "benchmark,run,ticks,us,cycles,pmu_cycles\n");
			for (i = 0; i < n; i++) {
				fprintf(f, "%s,%d,%llu,%.4f,%.1f,",
					BENCH_NAME, i + 1,
					(unsigned long long)ticks[i],
					ticks[i] * us_per_tick,
					ticks[i] * cyc_per_tick);
				if (pmu_ok)
					fprintf(f, "%llu\n", (unsigned long long)pmucyc[i]);
				else
					fprintf(f, "\n");
			}
			fclose(f);
		}
	}

	if (pmu_fd >= 0)
		close(pmu_fd);
	free(pmucyc);
	free(ticks);
	return 0;
}
