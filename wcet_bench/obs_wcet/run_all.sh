#!/usr/bin/env bash
#
# run_all.sh — measure the observed WCET of the Malardalen subset used in
# Figure 10 of Li et al. (2024), on a Raspberry Pi 5.
#
# Produces (in results/, or results-cold/ with -f):
#   observed_wcet.csv   one summary row per benchmark
#   raw_<bench>.csv     every individual run
#   env.txt             the machine state the numbers were taken under
#
set -euo pipefail

N=20
WARMUPS=1
CORE=3
COLD=0
OUT=""

usage() {
	cat <<EOF
usage: $0 [-n RUNS] [-w WARMUPS] [-c CORE] [-f] [-o OUTDIR]

  -n RUNS     measured executions per benchmark (default $N; use 5 to match the paper)
  -w WARMUPS  discarded executions before measuring (default $WARMUPS)
  -c CORE     core to pin to (default $CORE, the last of the Pi 5's four)
  -f          cold cache: evict L1/L2/L3 and invalidate the I-cache before each
              measured run. This is the regime static WCET analysis assumes;
              without it the numbers are steady-state throughput.
  -o OUTDIR   output directory (default results, or results-cold with -f)
EOF
}

while getopts "n:w:c:o:fh" opt; do
	case "$opt" in
		n) N=$OPTARG ;;
		w) WARMUPS=$OPTARG ;;
		c) CORE=$OPTARG ;;
		o) OUT=$OPTARG ;;
		f) COLD=1 ;;
		h) usage; exit 0 ;;
		*) usage; exit 2 ;;
	esac
done

# Separate directories by default so a cold run never overwrites a warm one.
if [ -z "$OUT" ]; then
	[ "$COLD" -eq 1 ] && OUT=results-cold || OUT=results
fi

BENCH_FLAGS=()
[ "$COLD" -eq 1 ] && BENCH_FLAGS+=(-f)

cd "$(dirname "$0")"

BENCHES=(insertsort insertsort1024 edn fft1 adpcm expint cnt matmult fir bs)

# vcgencmd lives outside the default PATH on some images and is absent on
# non-Pi hosts; every call is guarded.
vcg() {
	if command -v vcgencmd >/dev/null 2>&1; then
		vcgencmd "$@" 2>/dev/null || echo "unavailable"
	elif [ -x /usr/bin/vcgencmd ]; then
		/usr/bin/vcgencmd "$@" 2>/dev/null || echo "unavailable"
	else
		echo "unavailable"
	fi
}

# ---------------------------------------------------------------- privileges
# Everything below is best-effort. Without root the measurement still runs; it
# is just noisier, and the report says so rather than pretending otherwise.
if [ "$(id -u)" -eq 0 ]; then
	SUDO=""
	PRIV=1
elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
	SUDO="sudo"
	PRIV=1
else
	SUDO=""
	PRIV=0
fi

GOV_PATH="/sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor"
OLD_GOV=""
restore_governor() {
	if [ -n "$OLD_GOV" ]; then
		echo "$OLD_GOV" | $SUDO tee "$GOV_PATH" >/dev/null 2>&1 || true
		echo "restored governor: $OLD_GOV" >&2
	fi
}
trap restore_governor EXIT

# ------------------------------------------------------------------- build
echo "==> building (OPT=${OPT:--O0})" >&2
make --no-print-directory OPT="${OPT:--O0}"
mkdir -p "$OUT"

# --------------------------------------------------------------- isolation
FIFO_OK=0
GOV_OK=0
if [ "$PRIV" -eq 1 ]; then
	if [ -w "$GOV_PATH" ] || [ -n "$SUDO" ]; then
		OLD_GOV=$(cat "$GOV_PATH" 2>/dev/null || true)
		if echo performance | $SUDO tee "$GOV_PATH" >/dev/null 2>&1; then
			GOV_OK=1
		else
			OLD_GOV=""
		fi
	fi
	if $SUDO chrt -f 80 true 2>/dev/null; then
		FIFO_OK=1
	fi
fi

RUNNER=(taskset -c "$CORE")
if [ "$FIFO_OK" -eq 1 ]; then
	RUNNER=($SUDO chrt -f 80 taskset -c "$CORE")
fi

if [ "$PRIV" -eq 0 ]; then
	echo "==> no root: running without SCHED_FIFO and without the performance" >&2
	echo "    governor. Results are valid but noisier; re-run with sudo for" >&2
	echo "    tighter numbers." >&2
fi

# --------------------------------------------------------------- environment
# The Pi 5 throttles under sustained load, and a battery that throttled midway
# is silently invalid: the clock drops but nothing in the timing path notices.
# Sample the throttle word before and after so the run can be judged afterwards.
THROTTLE_BEFORE=$(vcg get_throttled)
TEMP_BEFORE=$(vcg measure_temp)

{
	echo "# Observed-WCET measurement environment"
	echo "date              : $(date -Is)"
	echo "host              : $(uname -n)"
	echo "kernel            : $(uname -srm)"
	echo "model             : $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
	echo "compiler          : $(${CC:-gcc} --version | head -1)"
	echo "opt level         : ${OPT:--O0}"
	echo "runs (N)          : $N"
	echo "warmups           : $WARMUPS"
	echo "cache state       : $([ "$COLD" -eq 1 ] && echo 'COLD (evicted before every measured run)' || echo 'warm (steady state)')"
	echo "pinned core       : $CORE"
	echo "SCHED_FIFO(80)    : $([ "$FIFO_OK" -eq 1 ] && echo yes || echo 'no (unprivileged)')"
	echo "governor forced   : $([ "$GOV_OK" -eq 1 ] && echo 'performance' || echo "no (left at $(cat "$GOV_PATH" 2>/dev/null || echo unknown))")"
	echo "cpufreq cur/min/max (kHz):" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_cur_freq 2>/dev/null || echo ?)/" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/cpuinfo_min_freq 2>/dev/null || echo ?)/" \
		"$(cat /sys/devices/system/cpu/cpu${CORE}/cpufreq/cpuinfo_max_freq 2>/dev/null || echo ?)"
	echo "isolcpus/nohz     : $(cat /proc/cmdline 2>/dev/null || echo unknown)"
	echo "throttled (pre)   : $THROTTLE_BEFORE"
	echo "soc temp (pre)    : $TEMP_BEFORE"
	# Explains after the fact why the pmu_* columns came back empty: at
	# perf_event_paranoid >= 3 an unprivileged process cannot open the counter
	# at all. Root bypasses it via CAP_PERFMON, which is why sudo matters here
	# beyond just SCHED_FIFO.
	echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
	echo
	echo "Timing source: CNTVCT_EL0 (ARMv8-A generic timer), read before and"
	echo "after each execution, per Li et al. 2024 Sec. 5. The cycle columns are"
	echo "derived as ticks * f_cpu / CNTFRQ_EL0. The pmu_* columns are real"
	echo "cycles counted with perf_event_open(PERF_COUNT_HW_CPU_CYCLES) and are"
	echo "the ones to trust when the two disagree."
	if [ "$COLD" -eq 1 ]; then
		echo
		echo "Cold mode: before each measured run the harness streams an 8 MiB"
		echo "buffer (past L1D, L2 and L3) and invalidates the I-cache over its"
		echo "own text segment. The branch predictor cannot be reset from EL0 and"
		echo "stays warm, so these are a lower bound on a true cold start."
	fi
} > "$OUT/env.txt"

cat "$OUT/env.txt" >&2
echo >&2

# ------------------------------------------------------------------ measure
CSV="$OUT/observed_wcet.csv"
"./bin/${BENCHES[0]}" --header > "$CSV"

for b in "${BENCHES[@]}"; do
	if [ ! -x "./bin/$b" ]; then
		echo "==> skipping $b: binary missing" >&2
		continue
	fi
	"${RUNNER[@]}" "./bin/$b" -n "$N" -w "$WARMUPS" -r "$OUT" ${BENCH_FLAGS[@]+"${BENCH_FLAGS[@]}"} >> "$CSV"
done

THROTTLE_AFTER=$(vcg get_throttled)
TEMP_AFTER=$(vcg measure_temp)
{
	echo "throttled (post)  : $THROTTLE_AFTER"
	echo "soc temp (post)   : $TEMP_AFTER"
} >> "$OUT/env.txt"

if [ "$PRIV" -eq 1 ] && [ -n "$SUDO" ]; then
	$SUDO chown -R "$(id -u):$(id -g)" "$OUT" 2>/dev/null || true
fi

# throttled=0x0 is the clean case. Any other value means the firmware capped
# the clock or the voltage at some point; bits 0-3 are live conditions, bits
# 16-19 latch that it happened since boot. Either way the numbers above were
# taken on a machine that was not running flat out.
if [ "$THROTTLE_AFTER" != "unavailable" ] && [ "$THROTTLE_AFTER" != "throttled=0x0" ]; then
	echo >&2
	echo "############################################################" >&2
	echo "## WARNING: the SoC reports throttling." >&2
	echo "##   before: $THROTTLE_BEFORE  ($TEMP_BEFORE)" >&2
	echo "##   after : $THROTTLE_AFTER  ($TEMP_AFTER)" >&2
	echo "## The core clock was capped at some point, so these timings" >&2
	echo "## understate the machine and the cycle conversion is off." >&2
	echo "## Let the board cool, add a fan or a heatsink, and re-run." >&2
	echo "############################################################" >&2
elif [ "$THROTTLE_BEFORE" != "$THROTTLE_AFTER" ]; then
	echo >&2
	echo "## WARNING: throttle state changed during the run" >&2
	echo "##   $THROTTLE_BEFORE -> $THROTTLE_AFTER" >&2
fi

# ------------------------------------------------------------------- report
echo >&2
echo "==> $CSV" >&2
awk -F, 'NR==1 { next }
{
	printf "  %-11s  mean %12.3f us  MAX %12.3f us  (max/mean %5.3f)  mean %14.1f cyc  MAX %14.1f cyc\n", \
	       $1, $8, $10, ($8 > 0 ? $10/$8 : 0), $12, $14
}' "$CSV" >&2

cat >&2 <<'EOF'

The "mean" column is the observed WCET as Li et al. define it (they average 5
runs). MAX is the largest execution actually seen and is the more honest
worst-case proxy -- neither is a sound upper bound, since measurement can only
ever report paths that happened to execute.
EOF

# ------------------------------------------------- comparison against Fig. 10
# The deliverable. Written to the results directory as well as the terminal so
# it travels with the CSVs it was generated from.
if command -v python3 >/dev/null 2>&1; then
	COLD_DIR=results-cold
	WARM_DIR=results
	[ "$COLD" -eq 1 ] && COLD_DIR="$OUT" || WARM_DIR="$OUT"
	echo >&2
	if ./compare.py -c "$COLD_DIR" -w "$WARM_DIR" > "$OUT/comparison.md" 2>/dev/null; then
		cat "$OUT/comparison.md" >&2
		echo >&2
		echo "==> $OUT/comparison.md" >&2
	else
		rm -f "$OUT/comparison.md"
		echo "==> compare.py found no results to join; run it by hand later" >&2
	fi
else
	echo "==> python3 not installed; skipping the comparison table." >&2
	echo "    Install it and run ./compare.py to produce comparison.md." >&2
fi
