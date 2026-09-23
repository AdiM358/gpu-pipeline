# Plan

This is the plan written before any code changed, with a status column that is
updated as phases land. The reasoning behind individual decisions lives in
[DESIGN_NOTES.md](DESIGN_NOTES.md).

## Starting point (commit `ab70289`, tagged `baseline`)

The pipeline is `axi_lite_s_intf -> vertex_fetch -> geom_engine -> persp_viewport
-> prim_assembly -> rasterizer -> pixel_map -> framebuffer`. All eight existing
testbenches build and exit 0 under Verilator 5.020, but `make all` only runs five
of them, and several "tests" print values without checking them.

### Broken (produces wrong results)

| # | Where | Problem |
|---|-------|---------|
| B1 | `rasterizer.sv` | `frag_z = v0_z` makes every pixel of a triangle the same depth, so depth testing between faces is wrong (why the cube is missing faces). |
| B2 | `rasterizer.sv` | `frag_x/frag_y` are driven from `curr_x/curr_y` *after* they advance, while `frag_valid` describes the pixel *before* it. Every fragment is reported one pixel to the right (or at the start of the next row). |
| B3 | `rasterizer.sv` | The last pixel of the bounding box is never emitted (`frag_valid <= 0` on the final step). |
| B4 | `rasterizer.sv` | Coverage is `all E >= 0 or all E <= 0` with no fill rule: pixels on shared edges are drawn twice, and zero-area triangles emit a line of pixels. |
| B5 | `rasterizer.sv` / `prim_assembly.sv` | Only vertex 0's colour is used; backface culling does not exist (the TODO is still in `prim_assembly`). |
| B6 | `axi_lite_s_intf.sv` | Writes happen and `BVALID` rises *before* the `AWREADY/WREADY` handshake, which violates AXI. `WSTRB` is ignored, AW and W must arrive in the same cycle, and reads are tied off in `gpu_top`. |
| B7 | `persp_viewport.sv` | `w <= 0` (vertex behind the eye) divides through anyway and produces garbage; `w == 0` silently maps to screen centre. |
| B8 | `prim_assembly.sv` | The vertex counter is never reset between draws, so one short draw misaligns every later triangle. |
| B9 | `gpu_top.sv` | `busy` only reflects the fetch unit, so there is no way to know when a frame is actually finished. |
| B10 | `gpu_top_tb.cpp` | Runs a fixed 250k cycles, checks nothing, always prints "passed". Reads the framebuffer through a hierarchical peek. |

### Blocks synthesis or timing

| # | Where | Problem |
|---|-------|---------|
| S1 | `framebuffer.sv` | 640x480x32 bit = 9.8 Mbit, about 300 RAMB36; the XC7Z020 has 140. |
| S2 | `gpu_top.sv` | Framebuffer read port tied to 0 / unconnected, so synthesis would delete the RAM. The Z-buffer lives in the testbench, not in hardware. |
| S3 | `persp_viewport.sv` | Three combinational 64-bit dividers plus two 64-bit multiplies in one cycle. |
| S4 | `rasterizer.sv` | Setup does six 64-bit multiplies (twice, duplicated for `e*` and `e*_row`) in one cycle. |
| S5 | `geom_engine.sv` | Sixteen 32x32 multipliers (roughly 64 DSP48) and a four-input 64-bit add in a single stage, even though the fetch bus can deliver at most one vertex every 4 cycles. |

### Weak (works, but a reviewer would flag it)

- `vertex_fetch` issues one 8-beat burst per vertex, waits for it, streams it, and only then issues the next. Half of each burst is unused padding words, `rresp` is ignored and `rlast` is only used as a shortcut.
- `pixel_map` takes 3 cycles per fragment (not pipelined).
- `vertex_fetch.sv` lacks `` `default_nettype none ``; lint waivers are sprinkled around instead of fixing unused signals.
- No golden model, no coverage, no CI, no synthesis flow, no register map, no driver.
- Generated `.ppm` files are committed in the repo root.

## Key decisions (details in DESIGN_NOTES.md)

1. **320x240, RGB565 colour + 16-bit Z, both on chip.** Each buffer is 76,800 x 16 bit = 1.23 Mbit, at least 38 RAMB36 each, 76 of 140 total. 640x480 at 16 bpp alone would need about 150. The resolution is a parameter. The colour buffer gets a real external read port (for a scan-out or DMA engine), and the testbench reads frames through that port rather than peeking.
2. **Fixed-point formats sized to need.** Screen coordinates are Q11.4 (16-bit, 4 sub-pixel bits) with a ±1024 px guard band, edge functions are 34-bit, depth is 16-bit unsigned, and colour is RGB888 per vertex, interpolated and packed to RGB565 at the ROP.
3. **Vertex format shrinks to 16 bytes** (x, y, z, rgb), so every fetched byte is used. Fetch uses 16-beat (4-vertex) bursts, a vertex FIFO, credit-based prefetch with up to 4 bursts outstanding, 4 KB boundary splitting, and sticky error reporting on `rresp`/`rlast` mismatches.
4. **Rate-matched geometry.** The 32-bit fetch bus caps input at one vertex per 4 cycles, so the transform uses 4 multipliers over 4 cycles instead of 16 (about 48 fewer DSPs, same throughput). Perspective divide becomes one fully pipelined reciprocal (32-stage restoring divider, 1 per cycle) followed by 3 multiplies instead of 3 dividers.
5. **Triangle setup as its own unit** (area, winding, cull, trivial reject, edge equations, attribute gradients via one normalised reciprocal and a single shared multiplier). It overlaps with rasterisation of the previous triangle through the valid/ready handshake.
6. **Rasterizer rewrite:** incremental edge functions with a top-left fill rule, incremental Z and Gouraud RGB using the same stepping, and span evaluation (`RAST_SPAN` pixels tested per cycle, empty spans skipped in one cycle, covered pixels emitted one per cycle). `RAST_SPAN=1` reproduces a plain pixel-per-cycle walk, which gives a measured before/after from the same RTL.
7. **ROP (renamed from `pixel_map`):** fully pipelined, 1 fragment per cycle, with Z read-after-write hazards resolved by 3-entry forwarding. It also contains the hardware clear engine (colour and Z, 1 pixel per cycle).
8. **Clipping scope:** trivial reject against all six frustum planes using clip-space outcodes. Triangles that cross the near plane (any `w <= 1/16`) or leave the guard band are **dropped and counted**, not clipped. Depth is clamped. Real near-plane clipping, which can split one triangle into two, is left out; see below.

## Phases

Every phase ends with the full regression green, a commit, and a DESIGN_NOTES entry. New datapath modules are built and unit-tested standalone first, and `gpu_top` is switched over in one phase, so no commit breaks the top-level test.

| Phase | Content | Status |
|-------|---------|--------|
| 0 | Plan (this file), `baseline` tag | done |
| 1 | Build/test infrastructure: generic Makefile (`make all` runs every test and fails on any failure), shared self-checking harness (no `assert`, which `-DNDEBUG` would disable), all existing tests converted to real checks, Docker image, stray PPMs removed | done |
| 2 | `axil_regs`: spec-compliant AXI-Lite (independent AW/W, WSTRB, reads, back-pressure), register map with STATUS/W1C, perf counters, `docs/REGMAP.md`, `sw/gpu_regs.h`, C++ driver used by the testbench | done |
| 3 | `vertex_fetch` rewrite: 16 B vertices, bursts, prefetch FIFO, credits, rresp/rlast, randomised-latency memory model | done |
| 4 | C++ golden model (per-stage functions plus a full `render()`); `geom_engine` rate-matched and pipelined; `recip_pipe` and new `persp_viewport` with outcode/near/guard-band flags; unit tests compare bit-exactly against the model with random stimulus and back-pressure | planned |
| 5 | `tri_setup` (cull, reject, gradients) and new `rasterizer` (Z, Gouraud, fill rule, span); unit tests compare fragment sets to the model | planned |
| 6 | `rop` with forwarding and clear, `sdp_ram`, external read port; switch `gpu_top` to the new datapath; delete old modules; system scene tests compare pixel-for-pixel and counter-for-counter to the model | planned |
| 7 | Coverage (`make coverage`, per-module report, tests for uncovered branches) and GitHub Actions | planned |
| 8 | Performance: `RAST_SPAN` sweep, old-vs-new unit benches built from the `baseline` tag, rotating-cube demo, PPM to GIF | planned |
| 9 | FPGA: Vivado batch OOC synth/impl, clock sweep, reports into `fpga/reports/`; Yosys cross-check if feasible | planned |
| 10 | README, METRICS, RESUME, final summary | planned |

## Deliberately left out

- **Near-plane clipping (triangle splitting).** It needs clip-space assembly before the divide, a second divider for the intersection parameter, and emitting up to two triangles per input. Triangles that cross the near plane are dropped and counted, and the demo keeps geometry in front of the camera. This is the most natural next step.
- **Texturing and perspective-correct interpolation.** Attributes are interpolated affinely in screen space, which is correct for Z (z/w is affine in screen space) and an accepted approximation for Gouraud colour.
- **Multiple fragments per cycle in the ROP.** A single-ported read-modify-write Z-buffer caps the backend at one fragment per cycle. The rasterizer's span logic is aimed at not wasting cycles on empty pixels, not at exceeding one fragment per cycle.
- **Hierarchical Z, tiling, MSAA, double buffering.** Double buffering would need 114 of 140 BRAMs; a single buffer plus a hardware clear is enough for offline rendering.
- **A Zynq block design, video output, or a bare-metal driver on real hardware.** There is no board, so nothing here is described as running on one. The C++ driver talks to an abstract register bus; the testbench is the only bus implementation.
- **An iterative/multi-cycle geometry divider.** Rejected because it would cap vertex rate at 1 per 32 cycles (see DESIGN_NOTES).
