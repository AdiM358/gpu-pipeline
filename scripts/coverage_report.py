#!/usr/bin/env python3
"""Per-module line-coverage report from a (merged) Verilator coverage.dat.

Verilator's --coverage-line instruments every statement block and branch
arm ("coverage points"). A point that is hit in any instance of a module
counts as covered. Prints a Markdown table (module, points hit / total, %)
plus every uncovered point, so gaps are visible rather than averaged away.

usage: coverage_report.py merged.dat
"""
import collections
import re
import sys


def parse(path):
    points = collections.defaultdict(int)  # (file, line, column, kind) -> hits
    rx = re.compile(r"^C '(.*)' (\d+)$")
    with open(path, encoding="latin-1") as f:
        for raw in f:
            m = rx.match(raw.rstrip("\n"))
            if not m:
                continue
            fields = dict(kv.split("\x02", 1) for kv in m.group(1).split("\x01") if "\x02" in kv)
            fname = fields.get("f", "")
            if "/rtl/" not in fname:
                continue
            key = (fname.split("/rtl/")[-1], int(fields.get("l", 0)), int(fields.get("n", 0)),
                   fields.get("o", ""))
            points[key] += int(m.group(2))
    return points


def main():
    points = parse(sys.argv[1])
    per_file = collections.defaultdict(lambda: [0, 0])
    missed = collections.defaultdict(list)
    for (fname, line, _col, kind), hits in sorted(points.items()):
        per_file[fname][1] += 1
        if hits:
            per_file[fname][0] += 1
        else:
            missed[fname].append(f"{line} ({kind})")

    print("| Module (file) | Points hit | Total | Line coverage |")
    print("|---|---:|---:|---:|")
    hit_all = tot_all = 0
    for fname in sorted(per_file):
        hit, tot = per_file[fname]
        hit_all += hit
        tot_all += tot
        print(f"| `{fname}` | {hit} | {tot} | {100.0 * hit / tot:.1f}% |")
    print(f"| **overall** | **{hit_all}** | **{tot_all}** | **{100.0 * hit_all / tot_all:.1f}%** |")
    print()
    if any(missed.values()):
        print("Uncovered points (line, kind):")
        print()
        for fname in sorted(missed):
            if missed[fname]:
                print(f"- `{fname}`: " + ", ".join(missed[fname]))
    else:
        print("No uncovered points.")


if __name__ == "__main__":
    main()
