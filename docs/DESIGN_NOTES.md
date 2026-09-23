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
