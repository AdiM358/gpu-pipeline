#!/usr/bin/env bash
# Compile and run the ROP UVM environment with the Vivado simulator (xsim).
#
#   tb/uvm/run_xsim.sh [test] [seed]
#   tb/uvm/run_xsim.sh rop_full_test 1
#
# Tests: rop_full_test (default), rop_random_test, rop_hazard_test,
#        rop_gap_test, rop_clear_draw_test, rop_fwd_directed_test.
# Output goes to build/uvm/<test>/ (git-ignored); the log is run.log.
# On Windows run it from Git Bash with Vivado's bin directory on PATH.
# ROP_RTL=<file> substitutes a different rop.sv (used for bug injection).
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
test="${1:-rop_full_test}"
seed="${2:-1}"
rop_rtl="${ROP_RTL:-$root/rtl/rop.sv}"

# On Windows the Vivado tools are .bat files.
tool() { if command -v "$1.bat" >/dev/null 2>&1; then echo "$1.bat"; else echo "$1"; fi; }
XVLOG=$(tool xvlog); XELAB=$(tool xelab); XSIM=$(tool xsim)

out="$root/build/uvm/$test"
mkdir -p "$out"
cd "$out"

"$XVLOG" -sv -L uvm -i "$root/tb/uvm" \
    "$root/rtl/sdp_ram.sv" "$rop_rtl" \
    "$root/tb/uvm/rop_if.sv" "$root/tb/uvm/rop_pkg.sv" "$root/tb/uvm/tb_top.sv" > compile.log 2>&1 \
    || { cat compile.log; exit 1; }
"$XELAB" -L uvm -timescale 1ns/1ps tb_top -s rop_uvm > elab.log 2>&1 \
    || { cat elab.log; exit 1; }
# Options go through a file: on Windows xsim is a .bat, and batch files split
# arguments at '=', which would break UVM_TESTNAME=<test>.
printf -- '--testplusarg UVM_TESTNAME=%s\n--sv_seed %s\n' "$test" "$seed" > xsim_opts.txt
"$XSIM" rop_uvm -R -f xsim_opts.txt > run.log 2>&1 || true

# Summary: scoreboard, coverage, errors, verdict.
grep -E "\[SB\]|\[COV\]|UVM_ERROR|UVM_FATAL|\[RESULT\]" run.log | grep -v "Report counts" || true
grep -q "TEST PASSED" run.log
