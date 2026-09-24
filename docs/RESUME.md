# Resume bullets

Three bullets, each followed by the source of every number in it. Only
claims the repository supports are used. Nothing here says the design ran on
hardware.

---

**3D Graphics Pipeline (SystemVerilog, Zynq-7020 FPGA)**

- Designed a fixed-function 3D graphics pipeline in SystemVerilog, from AXI4
  vertex fetch through transform, perspective divide, triangle setup with
  culling, and a span rasterizer with depth and Gouraud interpolation, to an
  on-chip depth-tested framebuffer. Closed timing at **100 MHz** post-route
  in Vivado using **18%** of the XC7Z020's LUTs.

  - *100 MHz:* Vivado 2026.1 post-route clock sweep; 10 ns met with WNS
    +0.253 ns, and 9.5, 9 and 8 ns failed (`fpga/reports/summary.md`).
  - *18% of LUTs:* 9,590 of 53,200 Slice LUTs, post-route utilization at
    10 ns (`fpga/reports/period_10ns/util.rpt`).

- Raised throughput with microarchitecture changes, measured in cycle-accurate
  simulation:
  - a pipelined depth-test unit with read-after-write forwarding
    (**3.0 → 1.0 cycles/pixel**);
  - credit-based AXI burst prefetch (**11.0 → 4.02 cycles/vertex**, and
    42.0 → 4.14 under 32-cycle memory latency);
  - span rasterization (**1.7–2.1x** faster draws).

  I also resolved a **−2.29 ns** timing violation by re-pipelining the
  viewport stage.

  - *3.0 → 1.0:* `make baseline-bench` (original `pixel_map`, 3.00) vs
    `make test-rop` (1.00).
  - *11.0 → 4.02 and 42.0 → 4.14:* `make baseline-bench` (11.00 / 42.00) vs
    `make test-vertex_fetch` (4.02 / 4.14).
  - *1.7–2.1x:* `make perf`, draw cycles at SPAN=1 vs SPAN=4 over three
    scenes (1.71x, 2.00x, 2.10x).
  - *−2.29 ns:* WNS −2.291 ns before the fix, +0.253 ns after, at 10 ns
    (`fpga/reports/before_viewport_split_10ns/`, `after_viewport_split_10ns/`).

- Built a bit-accurate C++ golden model and a self-checking Verilator
  regression: **12 tests**, constrained-random AXI timing, **98.9%** line
  coverage, and GitHub Actions CI. Every pixel of **65 rendered frames**
  matches the model. Wrote an AXI4-Lite register interface and a C++ driver,
  and the testbench itself uses that driver.

  - *12 tests:* `make all`.
  - *98.9%:* `make coverage`, 356 of 360 points.
  - *65 frames:* 17 frame comparisons in `tb/gpu_top_tb.cpp` plus 48 demo
    frames (`make demo`).
  - *CI:* `.github/workflows/regression.yml`. The same commands were checked
    in a clean Ubuntu 24.04 container. Confirm the workflow is green on
    GitHub before citing it.

---

## Notes for using these

- **Say "simulation" when asked about performance.** Throughput numbers are
  cycle counts from simulation. Frame rates are only projections: simulated
  cycles divided by the post-route Fmax.
- **"Post-route" means Vivado place-and-route reports,** not a running board.
- If a line is too long, drop the parenthetical sub-numbers first; keep the
  headline number and its unit.
- **Be ready to explain any number here in detail.** See
  [INTERVIEW_PREP.md](INTERVIEW_PREP.md), section 6.
