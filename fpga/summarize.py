#!/usr/bin/env python3
"""Collect Vivado run directories (fpga/build.tcl output) into Markdown.

usage: summarize.py fpga/reports > fpga/reports/summary.md

Every value printed is parsed from a Vivado report; runs that are missing
or failed are listed as such. Fmax is defined as 1000 / (fastest swept
period with post-route WNS >= 0) - the swept grid, not an extrapolation.
"""
import os
import re
import sys


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def util_row(text, name):
    """(used, available, util%) for a row of a Vivado utilization table."""
    if not text:
        return None
    for line in text.splitlines():
        cells = [c.strip() for c in line.split("|")]
        if len(cells) > 3 and re.match(re.escape(name) + r"\*?$", cells[1]):
            nums = [c for c in cells[2:] if re.fullmatch(r"[\d.]+", c)]
            if len(nums) >= 3:
                return nums[0], nums[-2], nums[-1]
    return None


def power(text):
    if not text:
        return None
    m = re.search(r"Total On-Chip Power \(W\)\s*\|\s*([\d.]+)", text)
    return m.group(1) if m else None


def load(run_dir):
    s = read(os.path.join(run_dir, "summary.txt"))
    if not s:
        return None
    info = dict(line.split(" ", 1) for line in s.strip().splitlines() if " " in line)
    util = read(os.path.join(run_dir, "util.rpt"))
    info["lut"] = util_row(util, "Slice LUTs")
    info["ff"] = util_row(util, "Slice Registers")
    info["bram"] = util_row(util, "Block RAM Tile")
    info["dsp"] = util_row(util, "DSPs")
    info["power"] = power(read(os.path.join(run_dir, "power.rpt")))
    return info


def fmt_util(u):
    return f"{u[0]} ({u[2]}%)" if u else "n/a"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "fpga/reports"
    runs = {}
    for d in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        if os.path.isdir(os.path.join(root, d)):
            runs[d] = load(os.path.join(root, d))

    print("# Vivado results (xc7z020clg400-1, out-of-context, post-route)\n")
    if not any(runs.values()):
        print("No Vivado runs found. Run `fpga/sweep.sh all` on a machine with Vivado.")
        return

    period = sorted(((float(r["period_ns"]), k, r) for k, r in runs.items()
                     if r and k.startswith("period_")), reverse=True)
    if period:
        print("## Clock-period sweep (RAST_SPAN=4)\n")
        print("| Period (ns) | WNS (ns) | Met | Logic levels (worst path) | LUT | FF | BRAM36 tiles | DSP | Power (W) |")
        print("|---:|---:|:-:|---:|---:|---:|---:|---:|---:|")
        best = None
        for p, _k, r in period:
            met = float(r["wns_ns"]) >= 0
            if met and (best is None or p < best):
                best = p
            print(f"| {p} | {r['wns_ns']} | {'yes' if met else 'no'} | {r.get('critical_logic_levels', 'n/a')} | "
                  f"{fmt_util(r['lut'])} | {fmt_util(r['ff'])} | {fmt_util(r['bram'])} | {fmt_util(r['dsp'])} | "
                  f"{r['power'] or 'n/a'} |")
        print()
        if best is None:
            print("**Fmax:** no swept period met timing.\n")
        else:
            print(f"**Fmax (fastest swept period with WNS >= 0): {1000.0 / best:.1f} MHz ({best} ns).**\n")

    span = sorted((int(r["rast_span"]), k, r) for k, r in runs.items() if r and k.startswith("span"))
    if span:
        print("## RAST_SPAN cost\n")
        print("| RAST_SPAN | Period (ns) | WNS (ns) | LUT | FF | DSP |")
        print("|---:|---:|---:|---:|---:|---:|")
        for s, _k, r in span:
            print(f"| {s} | {r['period_ns']} | {r['wns_ns']} | {fmt_util(r['lut'])} | {fmt_util(r['ff'])} | "
                  f"{fmt_util(r['dsp'])} |")
        print()

    mods = [(k, r) for k, r in runs.items() if r and (k.startswith("baseline_") or k.startswith("new_"))]
    if mods:
        print("## Baseline vs new modules (same period)\n")
        print("| Run | Period (ns) | WNS (ns) | Logic levels (worst path) | LUT | FF | DSP |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for k, r in sorted(mods):
            print(f"| {k} | {r['period_ns']} | {r['wns_ns']} | {r.get('critical_logic_levels', 'n/a')} | "
                  f"{fmt_util(r['lut'])} | {fmt_util(r['ff'])} | {fmt_util(r['dsp'])} |")
        print()

    missing = [k for k, r in runs.items() if r is None]
    if missing:
        print("Runs without a summary (failed or incomplete): " + ", ".join(missing))


if __name__ == "__main__":
    main()
