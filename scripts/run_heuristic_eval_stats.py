#!/usr/bin/env python3
"""
run_heuristic_eval_stats.py — Roda examples/example_heuristic_eval N vezes e agrega
as métricas (min/mean/max latency, jitter, misses) de cada heurística de bin-packing
(First Fit, Best Fit, Worst Fit — cada uma com e sem os critérios de ordenação de
período/utilização de Lupu et al. 2010) para identificar qual se comporta melhor com
mais consistência ao longo das rodadas.

Uso:
    python3 scripts/run_heuristic_eval_stats.py [--runs N] [--num-groups G] [--num-cores C]
                                                 [--capacity CAP] [--binary PATH] [--timeout SEC]
                                                 [--raw-out CSV] [--summary-out CSV]
                                                 [--progress-every N] [--sudo]

Cada rodada só usa a tabela final impressa no stdout do binário
("Latency — all subtasks"); os CSVs por-heurística que o próprio binário escreve
(latency_heuristic_*.csv) são sobrescritos a cada rodada e não são usados aqui.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import pandas as pd

HEURISTICS = [
    "First Fit", "Best Fit", "Worst Fit",
    "FF+PerAsc", "BF+PerAsc", "WF+PerAsc",
    "FF+PerDesc", "BF+PerDesc", "WF+PerDesc",
    "FF+UtilAsc", "BF+UtilAsc", "WF+UtilAsc",
    "FF+UtilDesc", "BF+UtilDesc", "WF+UtilDesc",
]
REPO_ROOT = Path(__file__).resolve().parent.parent


def parse_table(stdout: str):
    """Extrai, para cada heurística, as métricas da tabela final do stdout.

    Retorna dict heuristic -> dict(metrics) ou heuristic -> None (INFEASIBLE),
    apenas para heurísticas encontradas no stdout.
    """
    results = {}
    for line in stdout.splitlines():
        stripped = line.rstrip()
        for name in HEURISTICS:
            if not stripped.startswith(name):
                continue
            rest = stripped[len(name):].strip()
            if rest.startswith("INFEASIBLE"):
                results[name] = None
            else:
                parts = rest.split()
                # min mean max jitter misses jobs miss% '%' C0=.. C1=.. ...
                if len(parts) < 7:
                    break
                min_us, mean_us, max_us, jitter_us, misses, jobs, miss_pct = parts[0:7]
                results[name] = {
                    "min_latency_us": float(min_us),
                    "mean_latency_us": float(mean_us),
                    "max_latency_us": float(max_us),
                    "jitter_us": float(jitter_us),
                    "misses": int(misses),
                    "jobs": int(jobs),
                    "miss_pct": float(miss_pct),
                }
            break
    return results


def run_once(binary, num_groups, num_cores, capacity, timeout, use_sudo):
    cmd = (["sudo"] if use_sudo else []) + [str(binary), str(num_groups), str(num_cores), str(capacity)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", "-n", type=int, default=1000, help="Número de rodadas (default: 1000)")
    ap.add_argument("--num-groups", type=int, default=6, help="Argumento num_groups do binário (default: 6)")
    ap.add_argument("--num-cores", type=int, default=3, help="Argumento num_cores do binário (default: 3)")
    ap.add_argument("--capacity", type=float, default=1.0,
                     help="Capacidade por core repassada ao binário (default: 1.0; >1.0 para testar saturação)")
    ap.add_argument("--binary", type=Path, default=REPO_ROOT / "example_heuristic_eval",
                     help="Caminho do binário (default: ./example_heuristic_eval)")
    ap.add_argument("--timeout", type=float, default=30.0, help="Timeout por rodada em segundos (default: 30)")
    ap.add_argument("--raw-out", type=Path, default=REPO_ROOT / "heuristic_eval_runs.csv",
                     help="CSV com dados brutos por rodada×heurística")
    ap.add_argument("--summary-out", type=Path, default=REPO_ROOT / "heuristic_eval_summary.csv",
                     help="CSV com o resumo agregado por heurística")
    ap.add_argument("--progress-every", type=int, default=50, help="Intervalo de rodadas para log de progresso")
    ap.add_argument("--sudo", action="store_true", help="Roda o binário com sudo (SCHED_FIFO real)")
    args = ap.parse_args()

    if not args.binary.exists():
        sys.exit(f"Binário não encontrado: {args.binary} (rode `make example_heuristic_eval` primeiro)")

    rows = []
    failures = 0
    t_start = time.monotonic()

    for run_id in range(1, args.runs + 1):
        try:
            rc, stdout, stderr = run_once(args.binary, args.num_groups, args.num_cores, args.capacity,
                                           args.timeout, args.sudo)
        except subprocess.TimeoutExpired:
            failures += 1
            print(f"[run {run_id}] TIMEOUT — pulando rodada", file=sys.stderr)
            continue

        parsed = parse_table(stdout)
        if not parsed:
            failures += 1
            print(f"[run {run_id}] tabela não encontrada no stdout (rc={rc}) — pulando rodada", file=sys.stderr)
            if stderr.strip():
                print(stderr.strip()[-500:], file=sys.stderr)
            continue

        if len(parsed) < len(HEURISTICS):
            failures += 1
            missing = [n for n in HEURISTICS if n not in parsed]
            print(f"[run {run_id}] tabela incompleta (rc={rc}): faltam {missing} "
                  f"— processo provavelmente morreu no meio da rodada, pulando",
                  file=sys.stderr)
            if stderr.strip():
                print(stderr.strip()[-500:], file=sys.stderr)
            continue

        for name in HEURISTICS:
            metrics = parsed.get(name)
            if metrics is None:
                # Encontrado no stdout mas marcado INFEASIBLE — sem métricas usáveis.
                rows.append({"run_id": run_id, "heuristic": name, "feasible": False})
            else:
                rows.append({"run_id": run_id, "heuristic": name, "feasible": True, **metrics})

        if args.progress_every and run_id % args.progress_every == 0:
            elapsed = time.monotonic() - t_start
            rate = elapsed / run_id
            remaining = rate * (args.runs - run_id)
            print(f"[{run_id}/{args.runs}] {elapsed:6.1f}s decorridos, "
                  f"~{remaining:6.1f}s restantes, {failures} falhas", file=sys.stderr)

    if not rows:
        sys.exit("Nenhuma rodada produziu dados válidos.")

    df = pd.DataFrame(rows)
    df.to_csv(args.raw_out, index=False)
    print(f"\nDados brutos salvos em {args.raw_out} ({len(df)} linhas, {failures} rodadas com falha)")

    ok = df[df["feasible"] == True].copy()  # noqa: E712

    # -----------------------------------------------------------------
    # Resumo agregado por heurística: médias das métricas ao longo das
    # rodadas válidas.
    # -----------------------------------------------------------------
    summary = (
        ok.groupby("heuristic")
        .agg(
            runs_ok=("run_id", "count"),
            avg_min_latency_us=("min_latency_us", "mean"),
            avg_mean_latency_us=("mean_latency_us", "mean"),
            std_mean_latency_us=("mean_latency_us", "std"),
            avg_max_latency_us=("max_latency_us", "mean"),
            avg_jitter_us=("jitter_us", "mean"),
            std_jitter_us=("jitter_us", "std"),
            avg_misses=("misses", "mean"),
            total_misses=("misses", "sum"),
            total_jobs=("jobs", "sum"),
        )
        .reindex(HEURISTICS)
    )

    summary.to_csv(args.summary_out)
    print(f"Resumo salvo em {args.summary_out}\n")

    pd.set_option("display.float_format", lambda x: f"{x:.3f}")
    print(summary.to_string())

    best = summary["avg_mean_latency_us"].idxmin()
    print(f"\nMenor mean latency média: {best} ({summary.loc[best, 'avg_mean_latency_us']:.1f} us)")


if __name__ == "__main__":
    main()
