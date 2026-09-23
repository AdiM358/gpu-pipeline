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
