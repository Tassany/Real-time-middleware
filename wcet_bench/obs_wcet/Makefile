# Observed-WCET benchmark build (Li et al. 2024, Fig. 10 subset).
#
# One binary per benchmark: bin/<name> = ../<name>.c + harness.c.
# The Malardalen sources in the parent directory are never modified -- the
# benchmark's main() is renamed at compile time with -Dmain=bench_entry.

# insertsort1024 is the tenth entry and is not a Malardalen file: Li et al. say
# they "changed the length of the target reversed array in insertsort to 1024",
# so a row measuring the same work they measured needs that variant. It lives
# here rather than in ../ to keep the Malardalen drop byte-for-byte upstream.
BENCHES := insertsort insertsort1024 edn fft1 adpcm expint cnt matmult fir bs

SRCDIR  := ..
CC      ?= gcc
OPT     ?= -O0

# bs.c declares main() with an implicit int return type, which GCC 14 (Debian
# Trixie) promotes from warning to error. Older GCC (Bookworm ships 12) does not
# accept -fpermissive for C at all, so probe instead of assuming.
PERMISSIVE := $(shell printf 'int main(void){return 0;}\n' | \
	$(CC) -fpermissive -x c - -o /dev/null 2>/dev/null && echo -fpermissive)

# -fno-builtin keeps GCC from substituting its own memcpy/cos/fabs for the
# benchmark's code. fft1.c even defines its own static fabs(), and letting the
# compiler swap implementations in would change what we are timing.
COMMON := -std=gnu17 $(OPT) -fno-builtin -g

# -w on the benchmark side only: these are 2000-2005 sources and the warning
# noise would bury anything real coming out of harness.c.
#
# -fno-stack-protector: Debian and Ubuntu build GCC with -fstack-protector-strong
# on by default, which injects a canary load and compare into every function with
# a local array -- edn and fir both pick up a reference to __stack_chk_fail. That
# is compiler-injected code sitting inside the timed region and absent from the
# CFG a static analyser sees, so it is turned off to keep the measured program
# faithful to the source, per the Malardalen/Chronos convention.
BENCH_CFLAGS := $(COMMON) $(PERMISSIVE) -w \
	-fno-stack-protector \
	-DUPPSALAWCET \
	-include bench_shim.h \
	-Dmain=bench_entry

HARNESS_CFLAGS := $(COMMON) -Wall -Wextra

LDLIBS := -lm

BINS := $(addprefix bin/,$(BENCHES))

.PHONY: all clean force
all: $(BINS)

bin obj:
	@mkdir -p $@

# Everything depends on this stamp, whose contents are the full compiler command
# line. Without it, `make OPT=-O2` after a default build reports "nothing to be
# done" and silently re-measures the -O0 binaries while the run log claims -O2.
# The recipe rewrites the file only when the flags actually changed, so the
# timestamp -- and therefore the rebuild -- is triggered by content, not by
# every invocation.
FLAG_SIG := $(CC) | $(BENCH_CFLAGS) | $(HARNESS_CFLAGS) | $(LDLIBS)

obj/.flags: force | obj
	@printf '%s\n' '$(FLAG_SIG)' | cmp -s - $@ || \
		{ printf '%s\n' '$(FLAG_SIG)' > $@; echo "==> build flags changed, rebuilding"; }

# adpcm.c is ISO-8859-1 with CRLF line endings (accented Swedish comments).
obj/adpcm.bench.o: BENCH_CFLAGS += -finput-charset=ISO-8859-1

obj/%.bench.o: $(SRCDIR)/%.c bench_shim.h obj/.flags | obj
	$(CC) $(BENCH_CFLAGS) -c $< -o $@

# insertsort1024.c is in this directory, not $(SRCDIR). An explicit rule beats
# the pattern rule above, so no vpath games are needed; the flags are identical.
obj/insertsort1024.bench.o: insertsort1024.c bench_shim.h obj/.flags | obj
	$(CC) $(BENCH_CFLAGS) -c $< -o $@

obj/%.harness.o: harness.c obj/.flags | obj
	$(CC) $(HARNESS_CFLAGS) -DBENCH_NAME='"$*"' -c harness.c -o $@

.SECONDARY: $(addprefix obj/,$(addsuffix .bench.o,$(BENCHES))) \
            $(addprefix obj/,$(addsuffix .harness.o,$(BENCHES)))

bin/%: obj/%.bench.o obj/%.harness.o | bin
	$(CC) $(COMMON) $^ -o $@ $(LDLIBS)

clean:
	rm -rf bin obj
