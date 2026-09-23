# Design notes

One entry per phase: what was decided, why, and what it cost. These are the
questions I expect in a design review or interview, answered with reasons
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
multipliers: about 16 DSP48 instead of about 64 for 32x32 products. The DSP
counts come from Vivado's usual 4-DSP decomposition of a 32x32 multiply, and
the real numbers are in the Vivado report. Measured throughput: 400 vertices
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
