#!/usr/bin/env bash
# Vivado batch sweeps for the XC7Z020. Run from the repository root with
# Vivado (2020.1 or newer) on PATH:
#
#   fpga/sweep.sh period   [periods...]   # default design, clock-period sweep
#   fpga/sweep.sh span     [period]       # RAST_SPAN 1/2/4/8 at one period
#   fpga/sweep.sh baseline [period]       # old vs new modules, same period
#   fpga/sweep.sh all                     # all three, then the summary
#
# Each run writes fpga/reports/<run>/ (see fpga/build.tcl); fpga/summarize.py
# collects them into fpga/reports/summary.md. Fmax is the fastest swept
# period whose post-route WNS >= 0.
set -euo pipefail
cd "$(dirname "$0")/.."
REPORTS=fpga/reports
mkdir -p "$REPORTS"

vivado_run() {  # period span out_dir [top] [rtl_dir]
    local out=$3
    mkdir -p "$out"
    echo "=== $out"
    vivado -mode batch -nojournal -log "$out/vivado.log" -source fpga/build.tcl -tclargs "$@" \
        | grep -E "^DONE|ERROR|CRITICAL WARNING" || true
}

sweep_period() {
    local periods=("$@")
    [ ${#periods[@]} -eq 0 ] && periods=(10.0 8.0 7.0 6.5 6.0 5.5 5.0)
    for p in "${periods[@]}"; do vivado_run "$p" 4 "$REPORTS/period_${p}ns"; done
}

sweep_span() {
    local p=${1:-8.0}
    for s in 1 2 4 8; do vivado_run "$p" "$s" "$REPORTS/span${s}_${p}ns"; done
}

baseline_modules() {
    local p=${1:-10.0}
    local base=build/baseline_rtl
    rm -rf "$base" && mkdir -p "$base"
    for f in $(git ls-tree --name-only baseline rtl/); do git show "baseline:$f" > "$base/$(basename "$f")"; done
    for m in geom_engine persp_viewport rasterizer pixel_map; do
        vivado_run "$p" 4 "$REPORTS/baseline_${m}_${p}ns" "$m" "$base"
    done
    for m in geom_engine persp_viewport tri_setup rasterizer rop; do
        vivado_run "$p" 4 "$REPORTS/new_${m}_${p}ns" "$m" rtl
    done
}

case "${1:-all}" in
    period)   shift; sweep_period "$@" ;;
    span)     shift; sweep_span "$@" ;;
    baseline) shift; baseline_modules "$@" ;;
    all)      sweep_period; sweep_span; baseline_modules ;;
    *) echo "usage: $0 {period|span|baseline|all} [args]"; exit 1 ;;
esac
python3 fpga/summarize.py "$REPORTS" | tee "$REPORTS/summary.md"
