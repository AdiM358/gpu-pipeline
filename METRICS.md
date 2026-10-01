# Metrics

Every number here comes from a command that was run on this repository.
Each one has its **source**:

- **simulation**: Verilator 5.020 cycle counts, from the hardware performance
  counters or counted by the testbench;
- **post-route**: Vivado 2026.1 implementation, out-of-context, `xc7z020clg400-1`;
- **Yosys**: an open-source synthesis cross-check, not Vivado.

Nothing has been measured on a board.

Simulation numbers were measured at commit `919d982`, whose RTL is identical
to the commit Vivado implemented. Rerun them with the commands shown. Windows
users can run every `make` target through `scripts/docker-make.sh <target>`.

---

## 1. FPGA implementation (post-route, Vivado 2026.1)

Command: `fpga/sweep.sh period 10 9.5 9 8` (or `fpga\sweep.ps1` on Windows).
Reports: `fpga/reports/period_*/`; summary: `fpga/reports/summary.md`.

| Clock period | WNS | Met |
|---:|---:|:-:|
| 10.0 ns | +0.253 ns | yes |
| 9.5 ns | −0.951 ns | no |
| 9.0 ns | −0.863 ns | no |
| 8.0 ns | −2.213 ns | no |

**Fmax = 100 MHz**, the fastest swept period with post-route WNS ≥ 0.

Utilization at 100 MHz (`fpga/reports/period_10ns/util.rpt`):

| Resource | Used | Available | Utilization |
|---|---:|---:|---:|
| Slice LUTs | 9,590 | 53,200 | 18.03% |
| Slice registers | 9,317 | 106,400 | 8.76% |
| Block RAM tiles (RAMB36) | 98 | 140 | 70.00% |
| DSP48E1 | 36 | 220 | 16.36% |

Total on-chip power: **0.377 W** (`power.rpt`). This is Vivado's vectorless
estimate with default switching activity, not a measurement.

### Timing fix, before and after (post-route, 10 ns)

| | WNS | Failing endpoints | Worst path |
|---|---:|---:|---|
| Before (`fpga/reports/before_viewport_split_10ns/`) | −2.291 ns | 72 | `persp_viewport` depth/clamp stage, 22 logic levels, 12.223 ns |
| After (`fpga/reports/after_viewport_split_10ns/`) | +0.253 ns | 0 | `persp_viewport` V1 depth arithmetic, 18 logic levels, 9.760 ns |

The change split one pipeline stage in two (see DESIGN_NOTES, phase 9b).

---

## 2. Throughput of individual units (simulation)

| Unit | Workload | Result | Command |
|---|---|---|---|
| Vertex fetch | 256 vertices, zero-latency memory | 1,029 cycles, **4.02 cycles/vertex** (bus limit 4.0) | `make test-vertex_fetch` |
| Vertex fetch | 256 vertices, 32-cycle read latency | 1,060 cycles, **4.14 cycles/vertex**, 4 bursts in flight | `make test-vertex_fetch` |
| Transform (`geom_engine`) | 400 vertices back to back | 1,607 cycles, 4.02 cycles/vertex | `make test-geom_engine` |
| Perspective/viewport | 500 vertices back to back | 538 cycles (37-cycle latency, then 1 per cycle) | `make test-persp_viewport` |
| Triangle setup | rasterized triangles | 57-cycle latency (min = mean = max) | `make test-tri_setup` |
| Rasterizer, SPAN=1 | fixed 200-triangle workload, 30,645 fragments | 118,403 cycles, **0.259 fragments/cycle** | `make test-rasterizer_s1` |
| Rasterizer, SPAN=4 | same | 46,999 cycles, **0.652 fragments/cycle** | `make test-rasterizer` |
| Rasterizer, SPAN=8 | same | 35,469 cycles, **0.864 fragments/cycle** | `make test-rasterizer_s8` |
| Depth test + write (`rop`) | 76,800 fragments | 76,800 cycles, **1.00 cycle/fragment** | `make test-rop` |
| Clear | 76,800 pixels | 76,801 cycles | `make test-gpu_top` |

## 3. Before/after for the replaced units (simulation)

The original units were rebuilt from the `baseline` git tag and run on the
same workloads (`make baseline-bench`):

| Unit | Baseline | Now | Speed-up |
|---|---:|---:|---:|
| Vertex fetch, zero-latency memory | 11.00 cycles/vertex | 4.02 | 2.7x |
| Vertex fetch, 32-cycle read latency | 42.00 cycles/vertex | 4.14 | 10.1x |
| Depth test + framebuffer write | 3.00 cycles/fragment | 1.00 | 3.0x |

## 4. Rasterizer span width, full pipeline (simulation)

These are DRAW-only commands, so the clear is excluded; cycles come from
`PERF_CYCLES`. Every run is also compared with the golden model.
Command: `make perf` (writes `out/perf.md`).

| Scene | Rasterized triangles | Fragments | SPAN=1 | SPAN=2 | SPAN=4 | SPAN=8 |
|---|---:|---:|---:|---:|---:|---:|
| demo cube | 4 | 13,674 | 29,521 | 21,479 | 17,281 | 14,900 |
| random_3 | 85 | 54,610 | 151,362 | 101,549 | 75,687 | 62,068 |
| random_8 | 185 | 195,867 | 609,443 | 398,551 | 290,302 | 233,257 |

SPAN=1 → SPAN=4 (the default) is 1.71x, 2.00x and 2.10x faster respectively.

## 5. Frames (simulation, with projections)

Command: `make demo`. It renders 48 frames of two cubes (a CLEAR+DRAW plus a
second DRAW per frame).

| Metric | Value |
|---|---|
| Triangles submitted | 1,152 (690 culled) |
| Fragments generated | 855,201 |
| Cycles per frame (`PERF_CYCLES`, clear + both draws) | mean 100,665; min 96,455; max 108,833 |
| Of which the clear | 76,801 |
| **Projected frame rate at Fmax** | **≈ 993 frames/s** = 100 MHz ÷ 100,665 cycles |
| Projected peak fragment rate at Fmax | 100 M fragments/s (ROP accepts 1 per cycle) |

The projections combine simulated cycle counts with the post-route Fmax. They
are **not** measured on hardware.

## 6. Verification (simulation)

| Metric | Value | Command |
|---|---|---|
| Tests | 12, all passing; any failed check fails the build | `make all` |
| Line coverage | **98.9%** (356 of 360 points); 4 misses explained in DESIGN_NOTES phase 7 | `make coverage` |
| System frames compared with the golden model | 17 in `gpu_top_tb` + 48 demo frames; every pixel, depth value and counter matches | `make test-gpu_top`, `make demo` |
| Fill rule | 200-triangle mesh: 14,400 pixels, 0 written twice, 0 holes | `make test-rasterizer` |
| Interpolation precision | worst overshoot 0.0049 LSB over 97.8 M sliver-triangle fragments | model-only search (DESIGN_NOTES phase 7) |

## 7. Open-source synthesis cross-check (Yosys, not Vivado)

Command: `python3 fpga/yosys_report.py` in the `gpu-synth` Docker image (Yosys
0.33, sv2v 0.0.12, `synth_xilinx -family xc7 -flatten`). Full table:
`fpga/reports/yosys_summary.md`. These are cell counts only: no timing, and
Yosys maps differently from Vivado.

| Module | Original RTL (`baseline` tag) | Current RTL |
|---|---|---|
| `persp_viewport` | 40,296 LUT, 7,810 CARRY4 (three 64-bit combinational dividers) | 3,393 LUT, 2,362 FF, 18 DSP (pipelined reciprocal) |
| `geom_engine` | 939 LUT, 48 DSP48E1 | 361 LUT, 12 DSP48E1 |
| rasterizer | 1,977 LUT, 24 DSP48E1 | rasterizer 1,830 LUT, 11 DSP + `tri_setup` 2,521 LUT, 6 DSP |
| depth test / buffers | `pixel_map` 89 LUT, buffers off chip | `rop` 479 LUT + 76 RAMB36E1 (both buffers on chip) |

Full design, RAST_SPAN = 1 / 2 / 4 / 8: 10,273 / 10,751 / 10,901 / 11,563 LUT
and 37 / 37 / 48 / 81 DSP48E1; 76 RAMB36E1 in every case.
