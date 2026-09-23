# Out-of-context synthesis + implementation of one configuration for the
# XC7Z020 (xc7z020clg400-1), non-project batch mode.
#
#   vivado -mode batch -nojournal -nolog -source fpga/build.tcl \
#          -tclargs <period_ns> <rast_span> <out_dir> [top] [rtl_dir]
#
# Writes into <out_dir>: post-synthesis and post-route utilization, timing
# summary, logic-level distribution, power, and summary.txt with the
# post-route worst setup slack (WNS). fpga/sweep.sh drives the period sweep;
# fpga/summarize.py turns the reports into fpga/reports/summary.md.
#
# Out-of-context: gpu_top's ports are not tied to package pins, so timing is
# register-to-register inside the design (the ports would connect to Zynq PS
# AXI ports and a display/DMA block in a full system). No I/O delays are set.

if {[llength $argv] < 3} {
    puts "usage: -tclargs <period_ns> <rast_span> <out_dir> \[top\] \[rtl_dir\]"
    exit 1
}
set period  [lindex $argv 0]
set span    [lindex $argv 1]
set out_dir [lindex $argv 2]
set top     [expr {[llength $argv] > 3 ? [lindex $argv 3] : "gpu_top"}]
set rtl_dir [expr {[llength $argv] > 4 ? [lindex $argv 4] : "rtl"}]
set part    xc7z020clg400-1

file mkdir $out_dir
set_part $part

# ---------------------------------------------------------------- sources
read_verilog -sv [lsort [glob $rtl_dir/*.sv]]

# Clock constraint (generated so the period is a parameter of the run).
set xdc [file join $out_dir clock.xdc]
set fh [open $xdc w]
puts $fh "create_clock -name clk -period $period \[get_ports clk\]"
# Model the clock arriving through a global buffer, as it would in-context.
puts $fh "set_property HD.CLK_SRC BUFGCTRL_X0Y0 \[get_ports clk\]"
close $fh
read_xdc -mode out_of_context $xdc

# ---------------------------------------------------------------- synthesis
set generics {}
if {$top eq "gpu_top"} { set generics [list -generic RAST_SPAN=$span] }
synth_design -top $top -part $part -mode out_of_context {*}$generics
report_utilization    -file [file join $out_dir post_synth_util.rpt]
report_timing_summary -file [file join $out_dir post_synth_timing.rpt]

# ---------------------------------------------------------------- implementation
opt_design
place_design
phys_opt_design
route_design

report_utilization                     -file [file join $out_dir util.rpt]
report_utilization -hierarchical       -file [file join $out_dir util_hier.rpt]
report_timing_summary -max_paths 20    -file [file join $out_dir timing.rpt]
report_design_analysis -logic_level_distribution -file [file join $out_dir logic_levels.rpt]
# Vectorless estimate (default switching activity); no simulation activity.
report_power                           -file [file join $out_dir power.rpt]

set path [get_timing_paths -delay_type max -max_paths 1 -nworst 1]
set wns  [get_property SLACK $path]
set fh [open [file join $out_dir summary.txt] w]
puts $fh "top $top"
puts $fh "part $part"
puts $fh "rast_span $span"
puts $fh "period_ns $period"
puts $fh "wns_ns $wns"
puts $fh "critical_start [get_property STARTPOINT_PIN $path]"
puts $fh "critical_end [get_property ENDPOINT_PIN $path]"
puts $fh "critical_logic_levels [get_property LOGIC_LEVELS $path]"
puts $fh "vivado [version -short]"
close $fh
puts "DONE: period $period ns, WNS $wns ns"
