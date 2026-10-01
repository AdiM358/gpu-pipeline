# Design notes

One entry per phase: what was decided, why, and what it cost. These are the
questions I expect in a design review, answered with reasons
from this design rather than generic ones.

---

## Phase 1: Verification infrastructure

**Decision: tests self-check through a small harness (`tb/common/sim.h`), not `assert()`.**
`assert` disappears under `-DNDEBUG` and aborts on the first failure. The
harness counts failures, reports up to 25 of them with file:line and got/expected
values, and returns the exit code, so `make` fails when any check fails.
A test with zero executed checks also fails, which catches a test that
silently skips its body.

**Decision: `make all` runs every test, and lint is part of the build.**
Verilator runs with `-Wall`, where warnings are fatal, so a new lint warning
breaks the regression the same way a failed check does.

**Decision: `--x-initial unique` plus `randReset(2)`.**
Uninitialised flops start with random values instead of zero, which makes a
missing reset visible in simulation. Verilator's default of zero-initialising
everything hides that class of bug, and real silicon does not zero its flops.

**Decision: the toolchain is pinned to Ubuntu 24.04's Verilator 5.020, locally in Docker and in CI.**
Local runs and CI use the same simulator version, so a green local run means a
green CI run.

---

## Phase 2: Register interface, controller, driver

**Bug fixed: the old slave returned BVALID before the AW/W handshakes.**
It wrote the register and raised `BVALID` in the cycle it saw both valids, then
raised `AWREADY/WREADY` a cycle later. A spec-compliant interconnect can
drop `AWVALID` as soon as it sees the response, or never sees `AWREADY` at
all. It also ignored `WSTRB` and required AW and W in the same cycle.
`axil_regs` holds AW and W in independent one-entry registers, so any
arrival order works. It writes once both are held and no B is pending, and
only then raises `BVALID`. The BFM checks this rule on every write.

*Trade-off:* one write every two cycles, rather than a skid-buffered one per
cycle. Register writes are a few dozen per frame, so throughput here is
irrelevant, and the simpler design is easier to show correct.

**Decision: unmapped addresses return SLVERR.** A driver bug (wrong offset)
shows up as a bus error instead of a silently ignored write.

**Decision: DONE means the frame is written, not that fetch finished.**
The old design's `busy` was only the fetch unit, so software would read
the framebuffer while the rasterizer was still drawing. `gpu_ctrl` waits for
fetch to finish and then for every downstream stage to report idle for
several consecutive cycles. The system test checks that no framebuffer write
happens after DONE.

**Decision: DONE is sticky and write-1-to-clear, and a set wins over a
simultaneous clear.** Otherwise a completion that lands in the same cycle as
software acknowledging the previous one is lost, and software waits forever.
The unit test pulses `done_set` on exactly the clearing edge. A second pulse
one edge earlier must be cleared, which proves the alignment. This is a
classic interrupt-status race.

**Decision: performance counters in hardware, cleared per command.**
Cycles, vertices, triangles in/culled/clipped, fragments generated/passed,
and rasterizer-busy cycles. Every performance number in METRICS.md is read
from these registers by the same driver software would use. Nothing is
measured by peeking at internal signals.

**Decision: the driver talks to an abstract `RegBus`.** The testbench's
AXI-Lite BFM implements it, so the code that renders the regression scenes is
driver code rather than testbench-only pokes. No hardware implementation is
included, because none could be tested.

**Testbench infrastructure added:** cycle agents (`observe()` before the edge,
`drive()` after), an AXI-Lite master BFM with randomised handshake timing, and
an AXI4 read-memory agent with random ARREADY/RVALID, multiple outstanding
bursts, protocol checks, and error injection. Two harness bugs found along the
way: Verilator models must be destroyed before their context, and monitors
must ignore the random pre-reset state that `--x-initial unique` creates.

---

## Phase 3: Vertex fetch

**Decision: shrink the vertex from 32 bytes to 16 (x, y, z, colour).**
The old format padded each vertex with 4 unused words, and every burst
fetched them, so half the read bandwidth was wasted. On a 32-bit bus the
16-byte format caps throughput at exactly 4 cycles per vertex. That number
sizes the geometry stage in phase 4.

**Decision: multi-vertex bursts, a vertex FIFO, and credit-based prefetch.**
The old unit issued one burst, waited for all of it, streamed the vertex,
and only then issued the next request, so memory latency was fully exposed
on every vertex. Now a burst of up to 4 vertices (16 beats) is issued
whenever the 16-entry FIFO has room counting data already in flight.
Reserving space at request time means:

- `RREADY` can be tied high: returning data always has somewhere to go, so
  the unit never back-pressures the interconnect mid-burst;
- up to 4 bursts are in flight, which covers roughly 64 cycles of memory
  latency at full rate.

Measured (unit test, simulation): 256 vertices take 1029 cycles with a
zero-latency memory (4.02 cycles/vertex against a 4.0 bus limit) and 1060
cycles with a 32-cycle read latency (4.14 cycles/vertex), with 4 bursts in
flight at peak.

*Trade-off:* the FIFO is 16 x 128 bits, which is small enough for distributed
RAM, and its storage sits in a reset-free `always_ff` so it can infer as such.
A deeper FIFO hides more latency but costs LUTs; 16 already saturates the bus
at 32 cycles of latency.

**Decision: split bursts at 4 KB boundaries.** AXI forbids a burst from
crossing a 4 KB boundary. The burst length is `min(4, remaining, vertices
left before the boundary)`. The memory agent checks every burst, and a
directed test starts 2 vertices before a boundary (expected bursts: 2, 4, 4, 1).

**Decision: errors are sticky, abandon the draw, and drain the bus.**
A non-OKAY `RRESP` or an `RLAST` on the wrong beat pulses `error_set` once
(`STATUS.FETCH_ERR`), stops new requests, discards buffered vertices so no
partial data reaches the pipeline, and ends the job only when every
outstanding burst has returned. Ending earlier would let the next job receive
the tail of the failed job's data. Tests inject both error types and then
run a clean job.

**Verification:** a scoreboard checks every vertex, in order, across 60 random
jobs (random length, alignment, AR/R timing, read latency, output
back-pressure), plus directed boundary, error, and throughput tests.
It passes on 11 seeds.

---

## Phase 4: Golden model, geometry, perspective divide

**Decision: write the bit-accurate C++ model first and treat it as the spec.**
`model/gpu_model.{h,cpp}` defines every format, rounding step and
wrap-around. Unit tests compare each stage bit-exactly, and the system test
(phase 6) compares whole frames pixel by pixel. The model evaluates each
pixel directly from plane equations, while the RTL steps incrementally with
spans and pipelines. Because the two formulations differ, agreement tests the
RTL's incremental and traversal logic rather than a transcription of it.

**Decision: 4 multipliers over 4 cycles, not 16 in one (geom_engine).**
Phase 3 fixed the input rate at one vertex per 4 cycles, the 32-bit bus limit,
so a fully parallel 4x4 transform would sit idle 75% of the time. Computing
one output row per cycle is the same throughput with a quarter of the
multipliers. Yosys maps the baseline module to 48 DSP48E1 and this one to
12 (`fpga/yosys_report.py`; Vivado's own count comes from the phase 9 flow).
Measured throughput: 400 vertices
in 1607 cycles (4.02 cycles/vertex). The pipeline is operand select, then
multiply, then a product register (so synthesis can pack the DSP's M and P
registers), then pairwise sums, then the final sum. The old design did the
4-input 64-bit add in one stage.

*Only 48 product bits are kept.* The result is bits [47:16] of the sum, and a
sum's low 48 bits depend only on the operands' low 48 bits, so the upper 16
bits of every product can be dropped without changing the answer. The
model computes the same thing with 64-bit wrap-around, and the test includes
full-range matrices where the wrap actually happens.

**Decision: one reciprocal and three multiplies instead of three dividers.**
The old code had three combinational 64-bit dividers in one cycle, which is
hundreds of logic levels. Now `recip_pipe` computes floor(2^44 / w) once per
vertex, and x, y, z are multiplied by it.

**Decision: a pipelined divider (32 stages, 1 per cycle), not an iterative one.**
An iterative divider is about 100 flops against about 3000, but it would
cap geometry at 1 vertex per 32 cycles, 8x slower than fetch. In the setup unit
(phase 5) the same trade-off goes the other way. Rate-matching decides both.
The divider's payload (x, y, z, colour, flags) rides in a reset-free shift
register with one output tap, which maps to SRL32 LUTs rather than 127x32
flops. Measured: 500 vertices in 537 cycles, a 36-cycle latency followed by
1 per cycle.

**Decision: numeric formats sized to need.**

| Quantity | Format | Why |
|---|---|---|
| model/clip coordinates | Q16.16, 32-bit | inherited; register-programmable matrix |
| reciprocal | floor(2^44/w), 32-bit unsigned | w > 1/16 bounds it below 2^32 |
| NDC | Q.20 | 20 fraction bits is < 1/16 px error even at 1023 px |
| screen x/y | Q11.4, 16-bit signed | 4 sub-pixel bits (standard); ±1024 px guard band |
| depth | 16-bit unsigned | half the BRAM of 32-bit; see phase 6 |
| colour | RGB888 per vertex | bits 31:24 of the colour word are not stored |

**Decision: the near plane is w > 1/16, and near triangles are dropped, not
clipped.** `w <= 1/16` flags a vertex NEAR and substitutes a harmless divisor.
The triangle is then dropped in setup and counted in `PERF_TRI_CLIPPED`, not
drawn as garbage (baseline bug B7). Outcodes for all six clip planes are
computed here in clip space, where they are exact, with a 33-bit compare so
that `-w` cannot overflow for `w = INT32_MIN`. The test hits
`w = 1/16 - 1ulp, 1/16, 1/16 + 1ulp, 0, -1, INT32_MIN, INT32_MAX`, points
exactly on and one ulp outside the frustum planes, and both sides of the
guard band. It requires every flag bit to have been produced and checked at
least once.

**Testbench gotcha:** Verilator does not mask input ports narrower than the C
type. A random 32-bit colour driven into a 24-bit port kept its top byte all
the way to the output. Testbenches now mask inputs to port width.

---

## Phase 5: Triangle setup and rasterizer

**Bug fixed: flat depth (B1), off-by-one fragment coordinates (B2), dropped
last pixel (B3), double-drawn edges (B4), flat colour, no culling (B5).**
The rasterizer was rewritten. The system test now compares full frames
against the golden model, pixel for pixel, on 8 scenes (see phase 6 for the
final list).

**Decision: triangle setup is its own unit, separate from traversal.**
Everything that happens once per triangle lives in `tri_setup`: reject,
cull, area, edge equations, gradients. The rasterizer's inner loop is only
additions. Setup takes 57 cycles per rasterized triangle (measured) and
far fewer for culled or rejected ones. It overlaps with rasterization of the
previous triangle, because its result waits in an output register while the
next triangle is set up.

**Decision: one shared multiplier and an iterative reciprocal in setup.**
Setup runs 38 multiplies per triangle through a single 38x26 pipelined
multiplier, and 1/area comes from a 25-iteration restoring divider. This is
the opposite choice from the perspective stage (phase 4), for the same
reason: rate. Setup runs once per triangle and hides behind rasterization,
which takes hundreds of cycles for any triangle that matters, so spending
more hardware to make it faster buys nothing. The divider runs in parallel
with the 22 multiplies that don't need 1/area.

*Normalised reciprocal:* 1/area needs to be accurate for triangles from
1/256 px^2 up to about 2^31 sub-pixel^2 in area. Normalising, `R =
floor(2^55 / (A << clz(A)))`, gives a 24-bit mantissa for every size, and the
gradient shift `35 - clz(A)` restores the exponent.

**Decision: incremental Z and Gouraud colour, all stepped the same way.**
Each attribute (z, r, g, b) is a plane `a(x,y) = a0 + ax*dx + ay*dy`. Setup
computes `a0` at the first pixel and the per-pixel steps. The rasterizer only
adds, the same as for edge functions. Screen-space linear depth is exact for
z/w, which is affine in screen space. For colour it is the standard Gouraud
approximation (not perspective-correct; see Limitations).

*Why 38-bit accumulators can wrap harmlessly:* the rasterizer steps across
the whole bounding box, including uncovered pixels, where extrapolated
values can be huge. The accumulators are modular (mod 2^38), and every
covered pixel's true value fits in 38 bits, so the value is exact at every
pixel that is actually drawn, whatever happened in between. The model uses
explicit `wrap(…, 38)` to match, and a final clamp to the vertex min/max
absorbs sub-LSB rounding at edges.

**Decision: top-left fill rule.** Setup normalises every triangle to positive
winding (swapping v1/v2), so "inside" is always "all three edge functions
>= 0". Edges that are not top or left get a bias of -1, so a pixel exactly
on a shared edge belongs to exactly one triangle. Test: a jittered 10x10
mesh (200 triangles) with half its vertices exactly on pixel centres must
cover all 14,400 pixels with 0 double writes and 0 holes. It passes at every
SPAN value.

**Decision: span traversal (`RAST_SPAN` pixels tested per cycle).**
The ROP takes one fragment per cycle, so emitting more is pointless. The
waste in a bounding-box rasterizer is the cycles spent on *empty* pixels,
which is about half the box for a typical triangle. Stage T tests SPAN pixel
centres per cycle (SPAN copies of the edge adders) and skips an empty span
in one cycle. Stage S emits covered pixels one per cycle. T keeps skipping
empty spans while S is still emitting.

Measured (unit test, fixed 200-triangle workload, 30,645 fragments, no
back-pressure):

| SPAN | cycles | fragments/cycle |
|---:|---:|---:|
| 1 (plain bounding-box walk) | 118,403 | 0.259 |
| 4 (default) | 46,999 | 0.652 |
| 8 | 35,469 | 0.864 |

*Cost:* SPAN x 3 edge adders and comparators, plus SPAN-entry step tables for
four 38-bit attributes. SPAN=4 is the default as the knee of the curve; the
Vivado sweep in phase 9 prices each point.

*Constraint found by writing the code:* a new triangle may load only after
stage S has drained, because S reads the per-triangle step tables. Double
buffering the tables would remove this bubble (up to SPAN cycles per
triangle) at the cost of another table set.

**Decision: empty bounding boxes count as "clipped".** A triangle can survive
frustum reject and still cover no pixel centre, either because it is tiny or
because it only touches the guard band. Setup drops it and counts it with the
rejected triangles, which keeps `tri_in = culled + clipped + rasterized`.

**Decision: cull configuration is latched with each triangle.** Found by the
unit test, which changes `RASTER_CFG` between triangles. Reading the live
register three cycles after acceptance would apply the wrong mode to a
triangle in flight.

**Testbench lesson: outcomes are not in program order.** While a rasterizable
triangle waits in setup's output register, the next triangle can already be
culled. The scoreboard matches "output" to the oldest unresolved triangle
and cull/clip pulses to the oldest one not parked in the output register.

---

## Phase 6: ROP, on-chip buffers, integration

**Blocker fixed: the framebuffer did not fit and would have been deleted.**
640x480 at 32 bits is 9.8 Mbit, about 300 RAMB36, but the XC7Z020 has 140.
The old top also left the read port unconnected, so synthesis would have
removed the RAM entirely, and the Z-buffer lived in the testbench.

**Decision: 320x240, RGB565 colour and 16-bit depth, both in block RAM.**
Capacity arithmetic (a design calculation, not a synthesis result; the
Vivado report has the real count): each buffer is 76,800 x 16 bit =
1,228,800 bit. A RAMB36 holds 32 Kbit of data (2K x 18 used as 16), so each
buffer needs at least 76800 / 2048 = 37.5, i.e. 38 RAMB36, and both need 76
of 140 (54%). Alternatives:

| Option | Minimum RAMB36 | Verdict |
|---|---:|---|
| 640x480 RGB565 colour only (Z off chip) | 150 | does not fit |
| 320x240 RGB888 + 24-bit Z | ~114 | fits, leaves ~26 for everything else |
| 320x240 RGB565 + 16-bit Z | 76 | chosen: room to spare, 16-bit Z is adequate for these depth ranges |
| 320x240 + double-buffered colour | 114 | not needed for offline rendering |

320x240 is QVGA, which a display controller would scan-double to 640x480.
The resolution is a parameter (up to 1023 px, limited by Q11.4 coordinates).

**Decision: a real external read port, used by the testbench.** `fb_rd_addr ->
fb_rd_data` has 2-cycle latency and one address per cycle. Colour reads use
the colour RAM's dedicated read port and work at any time, which is what a
scan-out engine needs. Depth reads share the Z RAM's read port with the
depth test, so they are only valid while the GPU is idle (like
`glReadPixels` of depth after `glFinish`). Every frame the regression
checks is read through this port, with no hierarchical peeks into the RTL,
and the port also keeps synthesis from optimising the RAMs away.

**Decision: fully pipelined depth test with 3-entry forwarding, not stalls.**
The old `pixel_map` took 3 cycles per fragment. The ROP now accepts one per
cycle: address, then RAM read, then compare, then registered write. That
opens a read-after-write hazard. A fragment that reads a pixel within 3
cycles of an earlier fragment's write to the same pixel would see stale
depth, and that happens constantly along shared edges and in small
triangles. Two options:

- *Stall* on an address match: simple, but costs up to 3 cycles per hazard.
- *Forward* (chosen): compare the address with the writes decided in each of
  the last 3 cycles and take the newest match. It costs three 17-bit
  comparators and a mux.

The history is indexed by cycle, not by fragment, so bubbles do not break it.
The unit test streams fragments at 1–6 distinct pixels with random bubbles
and ties, which hits every forwarding distance, and it checks both buffers
against a sequential reference. Measured: 76,800 fragments accepted in
76,800 cycles.

*Why the extra register before the RAM read:* a 76,800-deep buffer is 38
BRAM36 primitives, so the read address fans out to all of them. Computing
`y*320 + x` and driving that fan-out in the same cycle would put an
adder chain in front of a high-fan-out net. Registering it costs one cycle of
latency, and the forwarding depth is unchanged (it depends only on the
read-to-write distance).

**Decision: a hardware clear engine, one pixel per cycle.** It writes the
clear colour and depth 0xFFFF through the same registered write port, and
its writes enter the forwarding history too. *Cost, measured:* 76,801 cycles,
which is most of the frame time for a simple scene (the full cube command is
100,294 cycles, clear included). This is the first thing I'd optimise next:
wider RAM words (2 or 4 pixels per write), or a per-tile "cleared" bit so the
clear becomes free and the first depth test in each tile reads the clear
value.

**Decision: DONE requires every stage to be idle, with exact idle signals.**
Each stage's `idle` covers its own registers, including the ROP's pending
write, so one idle cycle after fetch completes means every write has
committed. The system test checks that no framebuffer change and no
BUSY/DONE change happens after DONE.

**System test (all through the driver, frames read back through the port):**
clear only; 10 CLEAR+DRAW scenes (single triangle, interpenetrating
triangles, culled cube, clipping corner cases, 6 random scenes); a second
DRAW accumulating without CLEAR; random memory latency/back-pressure with a
jittered register bus (the frames must be identical); a command written while
busy (ignored); the IRQ pin; a bus error mid-draw (FETCH_ERR, abandoned
draw, clean recovery); and a partial trailing triangle. Every frame and
every counter matches the golden model exactly.

---

## Phase 7: Coverage and CI

**Decision: line (block/branch) coverage over the whole regression, reported
per module.** Every test is built with `--coverage-line`, and `make coverage`
merges the databases. `scripts/coverage_report.py` counts a point as covered
if it is hit in any instance, and lists every uncovered point by line rather
than only printing an average. Line coverage shows which code ran, not that
it was checked; the checking comes from the model comparisons.

**Result (make coverage, Verilator 5.020): 349 of 353 points, 98.9%.** The
four uncovered points, with evidence for each:

| Point | Why it is not covered |
|---|---|
| `gpu_ctrl.sv` `default:` of the state case | Unreachable: 5 enum states in 3 bits; only reachable by an upset flop. Kept as a recovery path. |
| `tri_setup.sv` `default:` of the state case | Same (7 states in 3 bits). |
| `rasterizer.sv` attribute clamp, low and high branches | Unreachable in practice. The clamp guards against rounding outside the vertex range. A model-only search over 2,946,815 worst-case sliver triangles (97.8 M fragments, extreme attribute values) found a worst overshoot of 0.0049 LSB, about 100x below the 0.5 LSB needed to trigger it. A sliver stress test was added to the RTL regression too. I kept the clamp because this is an empirical bound, not a proof. |

**Decision: CI runs the same toolchain as local runs.** GitHub Actions on
`ubuntu-24.04` installs the distro Verilator (5.020), which is the same
package as `docker/Dockerfile`. The workflow lints, runs the regression with
coverage (any failed check fails the job), publishes the coverage table to
the run summary, and then reruns the regression with a per-run seed. Every
randomised test prints its seed on its PASS/FAIL line, so a CI failure
reproduces locally with `make all SEED=<n>`.

---

## Phase 8: Performance and demo

All numbers here are cycle counts from simulation, read from the hardware
performance counters (system level) or counted by the unit testbench. They
become time only when divided by the post-route Fmax (phase 9).

**Before/after for the replaced units (`make baseline-bench`).** The original
`vertex_fetch` and `pixel_map` are built straight from the `baseline` git tag
and measured on the same workloads as their replacements' tests:

| Unit | Baseline | New | Speed-up |
|---|---:|---:|---:|
| vertex fetch, 256 vertices, ideal memory | 11.00 cycles/vertex | 4.02 | 2.7x |
| vertex fetch, 256 vertices, 32-cycle read latency | 42.00 cycles/vertex | 4.14 | 10.1x |
| depth test + framebuffer write, 76,800 fragments | 3.00 cycles/fragment | 1.00 | 3.0x |

The latency row shows the value of prefetch. The baseline pays the full
memory latency on every vertex, while the new unit keeps 4 bursts in flight
and stays near the bus limit.

**Rasterizer span width at system level (`make perf`).** These are DRAW-only
commands, with clear excluded, through the whole pipeline. Every run is also
compared with the model.

| Scene | SPAN=1 | SPAN=2 | SPAN=4 | SPAN=8 |
|---|---:|---:|---:|---:|
| demo cube (4 triangles rasterized, 13,674 fragments) | 29,520 | 21,478 | 17,280 | 14,899 |
| random_3 (85 rasterized, 54,610 fragments) | 151,361 | 101,551 | 75,695 | 62,076 |
| random_8 (185 rasterized, 195,867 fragments) | 609,442 | 398,550 | 290,301 | 233,256 |

Draw cycles; SPAN 1 to 4 is 1.71x, 2.00x and 2.10x faster respectively. The
default is SPAN=4, and SPAN=8 still gains 14–20% for twice the edge-test
hardware. Whether it pays off depends on its LUT cost and Fmax, which only the
Vivado sweep can tell.

**Where the frame time goes.** In the demo (two cubes, 48 frames),
`PERF_CYCLES` averages 100,663 cycles per frame, including the 76,801-cycle
clear. The hardware clear is the largest single cost, which is why it is at the
top of the future-work list.

**Demo (`make demo`).** It renders 48 frames of a rotating cube with a smaller
cube orbiting through it, as two draws per frame sharing one depth buffer,
with per-vertex colour, perspective and back-face culling. Every frame goes
through the driver, is compared with the golden model (all pixels match),
and is read back through the hardware port to `out/demo/*.ppm`.
`scripts/make_gif.py` converts the frames into `docs/img/demo.gif` without
touching pixel values (2x nearest-neighbour upscale, GIF palette).

---

## Phase 9: FPGA flow (no board, no Vivado)

**Vivado was not installed on the build machine.** `fpga/build.tcl`
(out-of-context synth, opt, place, phys_opt, route, then reports) and
`fpga/sweep.sh` (clock-period sweep, RAST_SPAN sweep, baseline-vs-new modules)
are written but **have not been run**. Fmax, Vivado utilization, timing and
power are therefore blank in METRICS.md. The commands are in `fpga/README.md`.
`fpga/summarize.py` was tested against a hand-written report in Vivado's format
(then discarded) and prints "no runs found" when there are none.

**Open-source cross-check that did run.** sv2v 0.0.12 + Yosys 0.33
(`docker/Dockerfile.synth`):

    sv2v --top=gpu_top rtl/*.sv > gpu_top.v
    yosys -p "read_verilog gpu_top.v; synth_xilinx -family xc7 -top gpu_top -flatten; stat"

Result for the default configuration (RAST_SPAN=4): **76 RAMB36E1**, which
matches the 2 x 38 capacity calculation; **48 DSP48E1**; 10,957 LUT (LUT1–LUT6,
excluding memory LUTs); 8,870 flip-flops; 33 RAM32M (the vertex FIFO, in
LUTRAM as intended); 299 SRL16E/SRLC32E (the divider payload shift register);
1,169 CARRY4. This shows the RTL synthesises and infers block RAM, DSPs, LUTRAM
and SRLs where the design intends. It is not timing and not Vivado's
mapping.

**Scripted matrix (`fpga/yosys_report.py`, results in
`fpga/reports/yosys_summary.md`).** It covers every RAST_SPAN and the baseline
modules next to their replacements. It reports 10,914 LUT for the default
design rather than 10,957: the script sets the parameter with `chparam`,
which re-elaborates the design, and Yosys optimises slightly differently.
Both counts are real outputs of their commands. The reproducible one is the
script's. Highlights:

| Module | Baseline | New |
|---|---|---|
| geom_engine | 939 LUT, 48 DSP | 361 LUT, 12 DSP |
| persp_viewport | 40,296 LUT, 7,810 CARRY4 (three 64-bit combinational dividers) | 3,393 LUT, 18 DSP, 2,257 FF (pipelined) |
| rasterizer | 1,977 LUT, 24 DSP | 1,830 LUT, 11 DSP, plus tri_setup at 2,521 LUT and 6 DSP |
| pixel_map / rop | 89 LUT, no memory | 479 LUT, 76 RAMB36E1 (both buffers now on chip) |

RAST_SPAN 1 / 2 / 4 / 8 costs 10,342 / 10,796 / 10,914 / 11,474 LUT and
37 / 37 / 48 / 81 DSP48E1 for the full design. Yosys maps the rasterizer's
k x step tables to DSPs at larger spans; Vivado may choose LUTs instead.

*A tool pitfall found here:* the first version of the script also reported
Yosys' `ltp -noff` as a logic-depth proxy. The full-design numbers
(1,193–1,400 cells) were implausible for a pipelined design, and a check on a
50-stage register pipeline (length 153 after `synth_xilinx`, 5 after generic
`synth`) showed that `ltp` walks through mapped Xilinx flip-flops. The column
was removed and not reported. Logic depth comes only from Vivado.

---

## Phase 9b: First Vivado results and a timing fix

Vivado 2026.1 (ML Standard) became available, so the flow in `fpga/` ran for
real. Command: `vivado -mode batch -source fpga/build.tcl -tclargs 10.0 4 <dir>`
(out-of-context, xc7z020clg400-1, post-route).

**The first implementation failed timing at 100 MHz.** WNS was -2.291 ns at a
10 ns period, with 72 failing endpoints. All of the 20 worst paths ran from
`persp_viewport`'s product register to its depth output: 22 logic levels (15
CARRY4). The viewport stage did the NDC shift, the depth scale
`(z/w + 1) * 65535`, a 64-bit shift, and the clamp and guard-band compares in
a single cycle. Two long carry chains were in series with the comparators.

**Fix: split that stage in two.** V1 registers the screen-coordinate and depth
arithmetic. V2 does the guard-band compare, the depth clamp and the output
register. The values fit in 48 bits (|sx|, |sy| < 2^31, |depth| < 2^34), so
the extra registers are exact. Latency goes up by one cycle (500 vertices:
537 -> 538 cycles). The unit test and the full-system pixel comparison still
pass bit-exactly.

**Result:** WNS went from -2.291 ns to **+0.253 ns** at 10 ns (100 MHz), with 0
failing endpoints. The evidence is in `fpga/reports/before_viewport_split_10ns/`
and `after_viewport_split_10ns/`. The worst remaining paths are the V1 depth
arithmetic (18 levels) and `vertex_fetch`'s `req_left` update (19 levels, +0.47 ns),
so those are the next targets if the clock needs to go higher.

**Post-route utilization at 100 MHz** (Vivado, `after_viewport_split_10ns/util.rpt`):
9,590 Slice LUTs (18.0%), 9,317 registers (8.8%), 98 Block RAM tiles (70.0%),
36 DSPs (16.4%). Vivado's vectorless power estimate is 0.377 W.

*BRAM finding:* Vivado uses 98 RAMB36 tiles for the two buffers, not the
theoretical minimum of 76 that Yosys achieved. A 76,800-deep memory is not a
power of two, and Vivado's default mapping wastes part of the depth. Splitting
each buffer by hand into a 64K-deep and a 12K-deep part would recover most of
it. That is future work; the design fits as is.

**Windows notes:** in PowerShell, `bash` is WSL's bash, which cannot see the
Windows Vivado install, and `python3` is the Microsoft Store placeholder.
`fpga/sweep.ps1` calls Git Bash explicitly, and `sweep.sh` picks the first
Python that actually runs.

**Clock sweep (`fpga/sweep.sh period 10 9.5 9 8`, results in
`fpga/reports/summary.md`):**

| Period | WNS | Met |
|---:|---:|:-:|
| 10.0 ns | +0.253 ns | yes |
| 9.5 ns | -0.951 ns | no |
| 9.0 ns | -0.863 ns | no |
| 8.0 ns | -2.213 ns | no |

**Fmax = 100 MHz**, the fastest swept period with WNS >= 0. The 10 ns run
reproduced the earlier result exactly (+0.253 ns): the flow is deterministic.
9.5 ns failing by more than 9 ns shows that place-and-route results near the
limit are not monotonic in the clock period.

What limits a faster clock differs by run. At 9.5 ns it is the V1 depth
arithmetic (`pz2 -> depth_v1`, two carry chains: `+2^20` then `x65535`). At
9 ns it is `vertex_fetch`: `req_left` and `req_addr` depend on the burst
length, which is a three-way minimum (4, vertices remaining, vertices to the
4 KB boundary) followed by a subtract in the same cycle. The next steps,
neither done yet:

1. Fold `+2^20` into the M2 register (`(p + 2^44) >>> 24 == (p >>> 24) + 2^20`,
   exact), leaving one carry chain in V1.
2. Register the burst length in vertex fetch one cycle ahead of issue.

**Numbers refreshed after the split** (all measurements re-run on commit
`919d982`; the full output is from `make all`, `make coverage`, `make perf`,
`make demo` and `make baseline-bench`). The extra pipeline cycle changes some
earlier figures slightly:

- coverage is 356/360 points (98.9%): the new stage added 7 points, all
  hit, and the same 4 misses remain;
- the demo averages 100,665 cycles per frame (was 100,663);
- each `make perf` draw is one cycle longer. For example, the demo cube at
  SPAN 1 / 4 takes 29,521 / 17,281 cycles.

The unit throughputs and the baseline benches are unchanged. METRICS.md
holds the current values.
