#!/usr/bin/env python3
"""wcet_latex_table.py -- CSV do measure_wcet.cpp para tabela LaTeX IEEE de 2 colunas.

Uso:
    python3 scripts/wcet_latex_table.py resultados_pi5.csv > tabela.tex
    python3 scripts/wcet_latex_table.py resultados_pi5.csv --label tab:obs_wcet_pi5 \
        --caption "Observed execution time on the Raspberry Pi 5"

O CSV so' tem "liq" (piso do instrumento descontado) pronto para a mediana, na
coluna mediana_pmucyc_liq. Media e maximo saem crus. Este script desconta o
piso das tres, subtraindo piso_pmucyc de media_pmucyc e de max_pmucyc, para
ficar consistente com a tabela da secao 4 do RELATORIO.md, que descontava o
piso das tres estatisticas.

Tempo (us) nao leva esse desconto. O relatorio so' fala em descontar o piso
para ciclos.
"""
import argparse
import csv
import sys


def formatar_milhar(n: float) -> str:
    return f"{n:,.0f}"


def escapar_latex(nome: str) -> str:
    return nome.replace("_", r"\_")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("--label", default="tab:obs_wcet")
    ap.add_argument("--plataforma", default=None,
                     help='nome da placa, por exemplo "Raspberry Pi 4". A legenda '
                          'e montada por dentro do script, e nao recebida pronta '
                          'pela linha de comando: o comando \\" do LaTeX tem uma '
                          "barra invertida que o shell engole se vier num "
                          "--caption solto.")
    ap.add_argument("--ordenar-por", choices=["ciclos_desc", "nome"],
                     default="ciclos_desc")
    args = ap.parse_args()

    legenda_base = r'Observed execution time of the M\"{a}lardalen benchmarks'
    if args.plataforma:
        args.caption = f"{legenda_base} on the {args.plataforma}."
    else:
        args.caption = f"{legenda_base}."

    linhas = []
    with open(args.csv_path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("pmu_ok") != "1":
                print(f"aviso: {row.get('benchmark')} sem ciclos de PMU (pmu_ok=0), pulei",
                      file=sys.stderr)
                continue
            if row.get("clock_estavel") != "1" or row.get("isolamento_completo") != "1":
                print(f"aviso: {row.get('benchmark')} com clock_estavel ou "
                      f"isolamento_completo != 1, pulei", file=sys.stderr)
                continue
            piso = float(row["piso_pmucyc"])
            ciclos_media = float(row["media_pmucyc"]) - piso
            ciclos_max = float(row["max_pmucyc"]) - piso
            linhas.append({
                "nome": row["benchmark"],
                "ciclos_media": ciclos_media,
                "ciclos_max": ciclos_max,
                "tempo_media": float(row["media_us"]),
                "tempo_max": float(row["max_us"]),
            })

    if args.ordenar_por == "ciclos_desc":
        linhas.sort(key=lambda r: r["ciclos_media"], reverse=True)
    else:
        linhas.sort(key=lambda r: r["nome"])

    print(r"\begin{table}[h]")
    print(r"    \centering")
    print(rf"    \caption{{{args.caption}}}")
    print(rf"    \label{{{args.label}}}")
    print(r"    \begin{tabular}{lrrrr}")
    print(r"        \hline")
    print(r"        & \multicolumn{2}{c}{\textbf{Cycles}} & \multicolumn{2}{c}{\textbf{Time ($\mu$s)}} \\")
    print(r"        \cline{2-3} \cline{4-5}")
    print(r"        \textbf{Benchmark} & \textbf{Mean} & \textbf{Max} & \textbf{Mean} & \textbf{Max} \\")
    print(r"        \hline")
    for r in linhas:
        nome = escapar_latex(r["nome"])
        print(f"        {nome:<14} & {formatar_milhar(r['ciclos_media']):>9} & "
              f"{formatar_milhar(r['ciclos_max']):>9} & "
              f"{r['tempo_media']:.3f} & {r['tempo_max']:.3f} \\\\")
    print(r"        \hline")
    print(r"    \end{tabular}")
    print(r"\end{table}")

    print(f"\n{len(linhas)} benchmarks na tabela", file=sys.stderr)


if __name__ == "__main__":
    main()
