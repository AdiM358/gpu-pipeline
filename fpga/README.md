# FPGA flow (XC7Z020, no board)

Target: Zynq-7020, `xc7z020clg400-1`. There is no board here. Nothing in
this repository has run on hardware, and all results are from simulation or
synthesis/implementation reports.

## Vivado (timing, Fmax, utilization, power)

Vivado was **not installed** on the machine this project was built on, so
the Vivado results in `METRICS.md` are blank until these scripts are run.
The scripts use only standard non-project batch commands, but they have not
been executed yet.

```sh
# from the repository root, Vivado 2020.1+ (free ML Standard edition supports the 7020)
fpga/sweep.sh all            # period sweep + RAST_SPAN sweep + baseline-vs-new modules
# or individually:
fpga/sweep.sh period 10 8 7 6.5 6 5.5 5
fpga/sweep.sh span 8.0
fpga/sweep.sh baseline 10.0
```

Each run: `synth_design -mode out_of_context`, `opt_design`, `place_design`,
`phys_opt_design`, `route_design`, then post-route utilization
(flat and hierarchical), timing summary, logic-level distribution and
vectorless power into `fpga/reports/<run>/`. `fpga/summarize.py` writes
`fpga/reports/summary.md`.

- **Fmax** = 1000 / (fastest swept period with post-route WNS >= 0). This is
  the swept grid, not extrapolated from slack.
- **Out-of-context:** the top-level ports are not placed on pins, so timing
  is register to register. In a full system they would connect to the Zynq
  PS (AXI GP/HP ports) and a display or DMA block.
- **Power** is Vivado's vectorless estimate (default toggle rates), not a
  measurement.
- **Baseline comparison:** `sweep.sh baseline` builds the original modules
  from the `baseline` git tag next to their replacements at the same clock.
  The original `gpu_top` is not comparable as a whole: its 640x480x32
  framebuffer does not fit the device, and its unconnected read port would
  let synthesis delete it.

## Yosys cross-check (runs without Vivado)

`fpga/yosys_report.py` converts the RTL with sv2v and maps it with Yosys
`synth_xilinx -family xc7`. It gives cell counts only: **no timing and no
logic depth**. It is here to show that the design synthesises, that the
buffers infer block RAM and the multipliers infer DSPs, and how resource use
changed against the baseline. Results: `fpga/reports/yosys_summary.md`.

(Yosys' `ltp -noff` was tried as a logic-depth proxy and dropped: after
`synth_xilinx` it does not stop at mapped flip-flops, so a 50-stage register
pipeline reports a path length of 153. Logic levels come from Vivado's
`report_design_analysis` instead.)

```sh
docker build -t gpu-synth -f docker/Dockerfile.synth docker
docker run --rm -v "$PWD":/work gpu-synth python3 fpga/yosys_report.py
```

Yosys and Vivado map differently (for example, Vivado absorbs more registers
into DSPs and BRAMs), so the two tools' counts are not interchangeable.
