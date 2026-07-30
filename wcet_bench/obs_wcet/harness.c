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
 *     the conversion stays auditable.
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
	"cpu_freq_hz,timer_freq_hz";

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-n RUNS] [-w WARMUPS] [-r RAWDIR] [--header]\n"
		"\n"
		"  -n RUNS      measured executions (default %d; the paper uses 5)\n"
		"  -w WARMUPS   discarded executions before measuring (default %d)\n"
		"  -r RAWDIR    write per-run samples to RAWDIR/raw_<bench>.csv\n"
		"  --header     print the CSV header line and exit\n"
		"\n"
		"stdout carries one machine-readable CSV row; the human-readable\n"
		"report and all warnings go to stderr.\n",
		argv0, DEFAULT_RUNS, DEFAULT_WARMUPS);
}

int main(int argc, char **argv)
{
	int n = DEFAULT_RUNS, warmups = DEFAULT_WARMUPS;
	const char *rawdir = NULL;
	uint64_t *ticks;
	uint64_t f_timer, f_cpu_before, f_cpu_after, f_cpu;
	uint64_t overhead = UINT64_MAX;
	struct stats st;
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

	/* Cost of the instrumentation itself, so it can be judged against the
	 * shorter benchmarks rather than assumed negligible. */
	for (i = 0; i < 1000; i++) {
		uint64_t a = rd_cntvct();
		uint64_t b = rd_cntvct();

		if (b - a < overhead)
			overhead = b - a;
	}

	ticks = malloc((size_t)n * sizeof(*ticks));
	if (!ticks) {
		fprintf(stderr, "harness: out of memory\n");
		return 1;
	}

	for (i = 0; i < warmups; i++)
		sink = bench_entry();

	for (i = 0; i < n; i++) {
		uint64_t t0, t1;

		t0 = rd_cntvct();
		sink = bench_entry();
		t1 = rd_cntvct();
		ticks[i] = t1 - t0;
	}
	(void)sink;

	f_cpu_after = cpu_freq_hz(cpu);
	f_cpu = f_cpu_before ? f_cpu_before : f_cpu_after;

	compute_stats(ticks, n, &st);

	us_per_tick  = 1e6 / (double)f_timer;
	cyc_per_tick = f_cpu ? (double)f_cpu / (double)f_timer : 0.0;

	/* ---- machine-readable row (stdout) ---- */
	printf("%s,%d,"
	       "%llu,%.2f,%.2f,%llu,"
	       "%.4f,%.4f,%.4f,%.4f,"
	       "%.1f,%.1f,%.1f,%.1f,"
	       "%llu,%llu\n",
	       BENCH_NAME, n,
	       (unsigned long long)st.min, st.mean, st.median, (unsigned long long)st.max,
	       st.min * us_per_tick, st.mean * us_per_tick,
	       st.median * us_per_tick, st.max * us_per_tick,
	       st.min * cyc_per_tick, st.mean * cyc_per_tick,
	       st.median * cyc_per_tick, st.max * cyc_per_tick,
	       (unsigned long long)f_cpu, (unsigned long long)f_timer);
	fflush(stdout);

	/* ---- human-readable report (stderr) ---- */
	fprintf(stderr,
		"[%s] n=%d warmups=%d core=%d f_cpu=%.3f GHz f_timer=%.3f MHz "
		"(%.3f ns/tick, read overhead %llu ticks)\n",
		BENCH_NAME, n, warmups, cpu,
		f_cpu / 1e9, f_timer / 1e6, 1e9 / (double)f_timer,
		(unsigned long long)overhead);
	fprintf(stderr,
		"[%s]   min %10.3f us | mean %10.3f us | median %10.3f us | MAX %10.3f us"
		"   (max/mean %.3f)\n",
		BENCH_NAME,
		st.min * us_per_tick, st.mean * us_per_tick,
		st.median * us_per_tick, st.max * us_per_tick,
		st.mean > 0.0 ? st.max / st.mean : 0.0);

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
	if (st.median < LOW_RES_TICKS)
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
			fprintf(f, "benchmark,run,ticks,us,cycles\n");
			for (i = 0; i < n; i++)
				fprintf(f, "%s,%d,%llu,%.4f,%.1f\n",
					BENCH_NAME, i + 1,
					(unsigned long long)ticks[i],
					ticks[i] * us_per_tick,
					ticks[i] * cyc_per_tick);
			fclose(f);
		}
	}

	free(ticks);
	return 0;
}
