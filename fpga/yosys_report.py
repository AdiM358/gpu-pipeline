#!/usr/bin/env python3
"""Open-source synthesis cross-check with sv2v + Yosys (synth_xilinx, xc7).

This is NOT Vivado and gives no timing: it reports mapped cell counts
(LUT/FF/CARRY4/DSP48E1/RAMB36E1/LUTRAM/SRL) and, as a structural proxy for
logic depth, Yosys' `ltp -noff` (the longest combinational path counted in
mapped cells). It is used to show that the RTL synthesises, that the
buffers infer block RAM and the multipliers DSPs, and how logic depth
changed versus the baseline RTL. Timing and Fmax come only from Vivado
(fpga/build.tcl).

Run inside the gpu-synth image (docker/Dockerfile.synth):
    python3 fpga/yosys_report.py [--out fpga/reports/yosys_summary.md]
"""
import argparse
import os
import re
import subprocess
import tempfile

DEVICE = {"LUT": 53200, "FF": 106400, "RAMB36E1": 140, "DSP48E1": 220}  # XC7Z020 (DS190)

# (label, source: "new" or "baseline", top, files, parameter overrides)
RUNS = [
    ("gpu_top RAST_SPAN=1", "new", "gpu_top", None, {"RAST_SPAN": 1}),
    ("gpu_top RAST_SPAN=2", "new", "gpu_top", None, {"RAST_SPAN": 2}),
    ("gpu_top RAST_SPAN=4 (default)", "new", "gpu_top", None, {"RAST_SPAN": 4}),
    ("gpu_top RAST_SPAN=8", "new", "gpu_top", None, {"RAST_SPAN": 8}),
    ("baseline geom_engine", "baseline", "geom_engine", ["geom_engine.sv"], {}),
    ("new geom_engine", "new", "geom_engine", ["geom_engine.sv"], {}),
    ("baseline persp_viewport", "baseline", "persp_viewport", ["persp_viewport.sv"], {}),
    ("new persp_viewport", "new", "persp_viewport", ["persp_viewport.sv", "recip_pipe.sv"], {}),
    ("baseline rasterizer", "baseline", "rasterizer", ["rasterizer.sv"], {}),
    ("new tri_setup", "new", "tri_setup", ["tri_setup.sv"], {}),
    ("new rasterizer (SPAN=4)", "new", "rasterizer", ["rasterizer.sv"], {}),
    ("baseline pixel_map", "baseline", "pixel_map", ["pixel_map.sv"], {}),
    ("new rop (incl. 2 buffers)", "new", "rop", ["rop.sv", "sdp_ram.sv"], {}),
]


def run(cmd, **kw):
    return subprocess.run(cmd, shell=True, check=True, text=True, capture_output=True, **kw).stdout


def stat_counts(log):
    block = log[log.rfind("Printing statistics"):]
    cells = dict((m.group(1), int(m.group(2))) for m in re.finditer(r"^\s+(\w+)\s+(\d+)\s*$", block, re.M))
    lut = sum(v for k, v in cells.items() if re.fullmatch(r"LUT[1-6]", k))
    ff = sum(v for k, v in cells.items() if re.fullmatch(r"FD[CPRS]E", k))
    lutram = sum(v for k, v in cells.items() if k.startswith("RAM32") or k.startswith("RAM64"))
    srl = sum(v for k, v in cells.items() if k.startswith("SRL"))
    return {"LUT": lut, "FF": ff, "CARRY4": cells.get("CARRY4", 0), "DSP48E1": cells.get("DSP48E1", 0),
            "RAMB36E1": cells.get("RAMB36E1", 0), "RAMB18E1": cells.get("RAMB18E1", 0),
            "LUTRAM": lutram, "SRL": srl}


def synth(src_dir, top, files, params, work):
    srcs = [os.path.join(src_dir, f) for f in files] if files else \
        sorted(os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(".sv"))
    v = os.path.join(work, top + ".v")
    run(f"sv2v --top={top} {' '.join(srcs)} > {v}")
    chparam = " ".join(f"chparam -set {k} {val} {top};" for k, val in params.items())
    script = (f"read_verilog {v}; {chparam} synth_xilinx -family xc7 -top {top} -flatten; "
              f"tee -o {work}/stat.txt stat; tee -o {work}/ltp.txt ltp -noff")
    run(f"yosys -q -p '{script}'", cwd=work)
    counts = stat_counts(open(f"{work}/stat.txt").read())
    m = re.search(r"Longest topological path in \S+ \(length=(\d+)\)", open(f"{work}/ltp.txt").read())
    counts["ltp"] = int(m.group(1)) if m else None
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="fpga/reports/yosys_summary.md")
    args = ap.parse_args()
    version = run("yosys -V").strip() + "; " + run("sv2v --version").strip()

    with tempfile.TemporaryDirectory() as tmp:
        base = os.path.join(tmp, "baseline_rtl")
        os.makedirs(base)
        for f in run("git -c safe.directory='*' ls-tree --name-only baseline rtl/").split():
            with open(os.path.join(base, os.path.basename(f)), "w") as fh:
                fh.write(run(f"git -c safe.directory='*' show baseline:{f}"))
        rows = []
        for label, src, top, files, params in RUNS:
            work = tempfile.mkdtemp(dir=tmp)
            try:
                c = synth(base if src == "baseline" else "rtl", top, files, params, work)
            except subprocess.CalledProcessError as e:
                print(f"{label}: FAILED\n{e.stderr[-2000:]}")
                raise
            rows.append((label, c))
            print(label, c, flush=True)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("# Yosys synthesis cross-check (not Vivado)\n\n")
        f.write(f"Tool: {version}. Command: `python3 fpga/yosys_report.py` in the gpu-synth image "
                "(docker/Dockerfile.synth). `synth_xilinx -family xc7 -flatten`, no timing. "
                "`ltp` = longest combinational path in mapped cells (LUT, CARRY4, MUXF, DSP ...), "
                "a structural proxy for logic depth, not a delay.\n\n")
        f.write("| Design | LUT | FF | CARRY4 | DSP48E1 | RAMB36E1 | LUTRAM cells | SRL | ltp (cells) |\n")
        f.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for label, c in rows:
            f.write(f"| {label} | {c['LUT']} | {c['FF']} | {c['CARRY4']} | {c['DSP48E1']} | "
                    f"{c['RAMB36E1']}{' + %d RAMB18' % c['RAMB18E1'] if c['RAMB18E1'] else ''} | "
                    f"{c['LUTRAM']} | {c['SRL']} | {c['ltp']} |\n")
        full = dict(rows)["gpu_top RAST_SPAN=4 (default)"]
        f.write("\nDefault configuration as a share of the XC7Z020 (capacities from DS190: "
                "53,200 LUTs, 106,400 FFs, 140 RAMB36, 220 DSP48E1). LUT excludes LUTs used as "
                "LUTRAM/SRL, so Vivado's 'Slice LUTs' will be higher:\n\n")
        for k in ("LUT", "FF", "RAMB36E1", "DSP48E1"):
            f.write(f"- {k}: {full[k]} / {DEVICE[k]} = {100.0 * full[k] / DEVICE[k]:.1f}%\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
