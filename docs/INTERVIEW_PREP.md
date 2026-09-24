# Interview preparation

A study guide for talking about this project. Read it top to bottom once,
then use the question bank at the end for practice out loud. Every number
here comes from a command that was actually run (simulation, Yosys, or
Vivado post-route). The source is given, so you can say where each number
comes from if asked.

**Contents**

1. [The 30-second and 2-minute pitch](#1-the-pitch)
2. [Owning the project](#2-owning-the-project)
3. [Architecture walkthrough, stage by stage](#3-architecture-walkthrough)
4. [The six stories to know cold](#4-the-six-stories-to-know-cold)
5. [Verification](#5-verification)
6. [Numbers and where they come from](#6-numbers-and-where-they-come-from)
7. [Fundamentals you must be able to explain](#7-fundamentals-refresher)
8. [Limitations and what you would do next](#8-limitations-and-next-steps)
9. [Question bank with answer outlines](#9-question-bank)
10. [Company-specific angles](#10-company-specific-angles)

---

## 1. The pitch

**30 seconds:**

> "I built a fixed-function 3D graphics pipeline in SystemVerilog, targeting a
> Zynq-7020 FPGA. It fetches triangles from memory over AXI, transforms them,
> does the perspective divide, culls and sets up triangles, rasterizes with
> depth and colour interpolation, and depth-tests into on-chip framebuffers.
> I verified it against a bit-accurate C++ model; every pixel of every test
> frame matches. It closes timing at 100 MHz in Vivado. The part I'm proudest
> of is the performance work: pipelining the depth test to one pixel per
> cycle, prefetching vertices, and fixing a timing failure Vivado found."

**2 minutes:** add one or two stories from section 4, depending on the role.
For verification roles, lead with the golden model and coverage. For RTL or
design roles, lead with timing closure and the ROP forwarding. For
architecture roles, lead with rate matching.

Be ready to say clearly: **it has only been simulated and synthesised; it
has not run on a board.** Say it before they ask.

---

## 2. Owning the project

Interviewers probe until they find the edge of your understanding. Before
any interview:

- **Be able to draw the pipeline from memory** (section 3), with the
  interface between each pair of stages.
- **Be able to explain every design decision in DESIGN_NOTES.md in your
  own words**, including the alternative you didn't pick and why.
- **Read the RTL of `rop.sv`, `rasterizer.sv` and `tri_setup.sv`** until you
  could re-derive them. These are the most likely deep-dive targets.
- **Be honest about process if asked.** Explain what you built, what you
  fixed, and which tools you used, including AI assistance if you used it.
  Interviewers care most whether you understand and can defend the design;
  getting caught overclaiming is far worse than any honest answer.
- **Never quote a number you can't source.** "I measured X with Y" beats a
  bigger number you can't explain.

---

## 3. Architecture walkthrough

```
CPU -AXI-Lite-> axil_regs -> gpu_ctrl
DDR -AXI4-> vertex_fetch -> geom_engine -> persp_viewport -> prim_assembly
            -> tri_setup -> rasterizer -> rop -> colour + depth BRAM -> read port
```

Every arrow between stages is a **valid/ready handshake**. A transfer
happens when both signals are high, and a stage holds its output until it
is taken, so a stall anywhere propagates backwards automatically.

| Stage | One-line job | Key idea to mention |
|---|---|---|
| `axil_regs` | CPU control over AXI4-Lite | AW and W accepted in any order, WSTRB byte strobes, SLVERR for bad addresses, sticky DONE with write-1-to-clear |
| `gpu_ctrl` | runs a command: CLEAR, then DRAW, then wait until empty, then DONE | DONE means every pixel is written, not just that the fetch finished |
| `vertex_fetch` | reads 16-byte vertices over AXI4 | up to 4 bursts in flight, FIFO space reserved before each request, 4 KB rule, clean error handling |
| `geom_engine` | 4x4 matrix times vertex (model space to clip space) | **rate-matched**: 4 multipliers over 4 cycles, because the bus only delivers 1 vertex per 4 cycles |
| `persp_viewport` | divide by w, map to pixels, compute clip flags | one pipelined reciprocal plus 3 multiplies instead of 3 dividers; flags for near plane and off-screen |
| `prim_assembly` | groups 3 vertices into a triangle | flushed at each draw, so a leftover vertex can't corrupt the next draw |
| `tri_setup` | per-triangle math: reject, cull, edges, gradients | one shared multiplier plus an iterative divider, because setup is not the bottleneck; 57-cycle latency, hidden behind rasterization |
| `rasterizer` | finds covered pixels, interpolates z and colour | tests 4 pixels per cycle, skips empty groups; add-only inner loop; top-left fill rule |
| `rop` | depth test and framebuffer write | 1 fragment per cycle with **3-entry forwarding** for read-after-write hazards; hardware clear |

**Number formats (know these):**

| Quantity | Format |
|---|---|
| model/clip coordinates | Q16.16, 32-bit |
| screen x, y | Q11.4, 16-bit (1/16-pixel precision) |
| depth | 16-bit unsigned |
| edge functions | 34-bit |
| attribute accumulators | 38-bit, 20 fraction bits, allowed to wrap |
| colour | RGB888 in, RGB565 in memory |

---

## 4. The six stories to know cold

Tell each one as **problem → options → decision → evidence**.

### Story 1: Timing closure at 100 MHz (the best "real engineering" story)

- **Problem:** The first Vivado run missed 100 MHz by 2.291 ns (WNS −2.291 ns,
  72 failing endpoints). The worst path had 22 logic levels, all inside the
  last stage of `persp_viewport`.
- **Diagnosis:** In one cycle, that stage shifted the product, added 2^20,
  multiplied by 65535 (a wide subtract), shifted again, then did
  greater/less-than comparisons for the clamp and the guard band. That's two
  long carry chains plus comparators in series. The timing report showed the
  path: 15 CARRY4 cells, 12.2 ns of delay against a 10 ns budget.
- **Fix:** Split the stage in two. Stage V1 does the arithmetic and registers
  the results; stage V2 does the compares, the clamp and the output. Each
  cycle now has roughly half the logic.
- **Why it's safe:** It's the same math, just registered in the middle. I
  proved the intermediate values fit in 48 bits, so nothing is lost. The
  bit-exact regression still passes on every test. The only cost is 1 extra
  cycle of latency (500 vertices: 537 → 538 cycles), and throughput is still
  one vertex per cycle.
- **Result:** WNS +0.253 ns at 10 ns, 0 failing endpoints.
- **What's next:** The next-worst paths are the depth arithmetic in V1
  (18 levels) and vertex fetch's `req_left` update (19 levels). One further
  fix would be to fold the "+2^20" into the previous register stage, so V1
  has one carry chain instead of two.
- **Concept to state:** *Pipelining trades latency for clock frequency. In a
  streaming design, throughput stays the same, so it's nearly free.*

### Story 2: The ROP read-after-write hazard (forwarding)

- A depth test is read-modify-write: read the old depth, compare, write the
  new one. Pipelined to one fragment per cycle, the read for fragment N+1
  happens before fragment N's write reaches the RAM. If both are the same
  pixel, N+1 compares against stale depth, which gives wrong visibility.
- **Options:** stall when the addresses match (simple, but loses cycles
  exactly when triangles are small or share edges), or **forward**, as a CPU
  does with register values.
- **Decision:** keep the last 3 cycles of writes (address, depth) in a small
  history. At compare time, check the newest match first, then fall back to
  the RAM. There are 3 entries because the write commits 4 cycles after the
  read address is issued.
- The history is indexed **by cycle, not by fragment**, so gaps in the
  stream don't break it.
- **Evidence:** the unit test aims fragments at 1–6 pixels, with random gaps
  and equal depths, and compares against a sequential reference. Result:
  76,800 fragments in 76,800 cycles, where the old design took 3.00 cycles
  per fragment.

### Story 3: Rate matching (geometry vs setup)

- The 32-bit AXI bus delivers one 16-byte vertex every 4 cycles, so a
  16-multiplier transform (one vertex per cycle) would sit idle 75% of the
  time. I use 4 multipliers and one row per cycle: the same throughput with a
  quarter of the DSPs (Yosys: 48 → 12).
- The **same principle** drives two opposite divider choices:
  - The perspective divider is **pipelined** (32 stages, 1 per cycle), because
    an iterative one would cap geometry at 1 vertex per 32 cycles, 8 times
    slower than fetch.
  - The setup divider is **iterative** (25 cycles), because setup runs once
    per triangle and hides behind rasterization.
- *"Match hardware to the rate it has to sustain"* is the line to remember.

### Story 4: Correct rasterization (fill rule, interpolation, wrap-around)

- **Bugs I fixed in the original:** every pixel got vertex 0's depth,
  fragments came out one pixel off, the last pixel was dropped, and shared
  edges were drawn twice.
- **Top-left rule:** normalize every triangle to the same winding, so
  "inside" means all 3 edge functions are ≥ 0. Then subtract 1 on edges that
  are neither top nor left. A pixel exactly on a shared edge belongs to
  exactly one triangle.
  - Test: a 200-triangle mesh, half its vertices exactly on pixel centres,
    covers 14,400 pixels with 0 double writes and 0 holes.
- **Interpolation:** each attribute (z, r, g, b) is a plane. Setup computes
  the start value and x/y step once, and the rasterizer only adds.
- **Why 38-bit wrap-around is fine:** stepping across empty parts of the
  bounding box can overflow, but addition is exact modulo 2^38. Every pixel
  that's actually drawn has a true value that fits, so it comes out exact.
- **Span traversal:** test 4 pixel centres per cycle and skip an empty group
  in 1 cycle. The ROP only accepts 1 pixel per cycle, so the win is from not
  wasting cycles on empty pixels, not from emitting more. Unit throughput:
  0.259 → 0.652 fragments/cycle (SPAN 1 → 4).

### Story 5: Vertex prefetch

- **Original:** request, wait, use, repeat, which paid the full memory
  latency on every vertex, and half of each burst was padding.
- **New:** 16-byte vertices, 4-vertex bursts, up to 4 bursts in flight.
  FIFO space is reserved before each request, so the unit can always accept
  data (`RREADY` tied high).
- **Measured:** 11.00 → 4.02 cycles/vertex with fast memory, and
  42.00 → 4.14 with 32 cycles of read latency. That's close to the 4.0
  bus limit.
- **Also:** bursts are split at 4 KB boundaries (an AXI rule), and a bus
  error ends the draw cleanly after draining outstanding bursts.

### Story 6: Verification with a golden model

- The C++ model is the **spec**: it defines every rounding step and
  wrap-around.
- The model computes each pixel directly from plane equations; the RTL steps
  incrementally, in spans, pipelined. **Two different formulations agreeing
  is strong evidence**, because I didn't just translate the RTL into C++.
- Unit tests compare each stage bit-exactly under random inputs and random
  back-pressure. The system test compares every pixel, every depth value and
  every counter on 17 system-test frames, plus 48 demo frames.
- Coverage is 98.9% (356/360). I can explain each of the 4 misses: two are
  unreachable FSM default branches, and two are a clamp that is unreachable
  in practice. A search of 97.8 million sliver-triangle fragments found a
  worst overshoot of 0.0049 LSB, about 100 times below where the clamp
  matters.

---

## 5. Verification

**What was verified, and how:**
- 12 self-checking tests. `make all` fails on any failed check, and lint
  warnings are errors.
- A bit-accurate C++ reference model compared at every stage and at the full
  frame.
- Random stimulus with seeds: every result line prints its seed, so any
  failure reproduces.
- Protocol checks inside the bus models:
  - the AXI master holds ARVALID and ARADDR until the handshake;
  - no burst crosses a 4 KB boundary;
  - BVALID never comes before the AW/W handshakes.
- Uninitialised flops start random (`--x-initial unique`), which exposes
  missing resets.
- Directed corner cases:
  - bus errors and wrong `rlast`;
  - near-plane and guard-band boundaries (exact and ±1 unit);
  - degenerate, sliver and huge triangles;
  - the W1C race;
  - partial triangles;
  - commands issued while busy.
- The testbench runs the real C++ driver over an AXI-Lite BFM, so the
  software path is tested too.

**Bugs the testbench found (good to mention):**
- **Cull configuration read too late.** `tri_setup` read the cull
  configuration 3 cycles after accepting the triangle, so a register change
  in between applied to the wrong triangle. Fixed by latching it with the
  triangle.
- **Out-of-order outcomes.** A culled triangle can report before an older
  triangle leaves setup's output register, so the scoreboard had to stop
  assuming results arrive in order.
- **Harness teardown.** The Verilator model must be destroyed before its
  context. Getting this wrong made the test hang.
- **Verilator doesn't mask inputs.** A 32-bit value driven into a 24-bit
  port kept its top bits.

**Tool pitfall found and rejected:** Yosys' `ltp` "longest path" command
looked like a logic-depth measure, but after Xilinx mapping it walks
through registers. A 50-stage register chain reported a length of 153. The
numbers were dropped rather than published.

**What is NOT verified:** real hardware, the Zynq PS integration, and
multiple clock domains (there is only one clock).

---

## 6. Numbers and where they come from

| Claim | Number | Source |
|---|---|---|
| Timing closure / Fmax | WNS −2.291 ns → +0.253 ns at 10 ns; **Fmax 100 MHz** (9.5, 9, 8 ns fail) | Vivado 2026.1 post-route, `fpga/reports/*_viewport_split_10ns/` |
| Utilization at 100 MHz | 9,590 LUT (18.0%), 9,317 FF (8.8%), 98 BRAM tiles (70.0%), 36 DSP (16.4%) | Vivado post-route `util.rpt` |
| Power | 0.377 W | Vivado *vectorless estimate*, not measured |
| Vertex fetch | 11.00 → 4.02 cycles/vertex; 42.00 → 4.14 with 32-cycle latency | simulation, `make baseline-bench` vs `test-vertex_fetch` |
| Depth test / write | 3.00 → 1.00 cycles/fragment | simulation, same benches |
| Rasterizer unit | 0.259 / 0.652 / 0.864 fragments/cycle (SPAN 1 / 4 / 8) | simulation, `test-rasterizer*` |
| System draw speed-up, SPAN 1 → 4 | 1.71x (demo cube), 2.10x (dense scene) | simulation, `make perf` |
| Setup latency | 57 cycles | simulation, `test-tri_setup` |
| Clear | 76,801 cycles | simulation, `test-gpu_top` |
| Demo frame | mean 100,665 cycles/frame (clear + 2 cubes), 48 frames | simulation, `make demo` |
| Frame rate projection | ~993 frames/s at 100 MHz (100 MHz ÷ 100,665 cycles) | **projection**: simulated cycles ÷ a clock that met timing |
| Resource reduction | `persp_viewport` 40,296 → 3,393 LUT; `geom_engine` 48 → 12 DSP | Yosys cross-check (not Vivado) |
| Coverage | 98.9% line (356/360 points) | `make coverage` |
| Correctness | every pixel and counter matches the model on 17 system-test frames + 48 demo frames | simulation |

How to phrase the frame rate: *"About a thousand frames per second for the
demo scene, projected from simulated cycle counts at 100 MHz. Most of that
time is the screen clear."*

---

## 7. Fundamentals refresher

Be able to explain each of these in 2–3 sentences.

**Digital design**
- **Pipelining:** registers between logic stages. It shortens the longest
  path per cycle, raising clock speed, at the cost of latency.
- **Setup slack, WNS and TNS:** slack is the required arrival time minus the
  actual arrival time. WNS is the worst slack; TNS is the sum of all negative
  slack. WNS ≥ 0 means timing is met.
- **Logic levels:** the number of LUT/carry cells in series on a path. Each
  adds delay, and routing adds more; here routing was about 45% of the path
  delay.
- **Valid/ready handshake:** a transfer happens when both are high; valid
  must not depend on ready. Know how back-pressure works, and what a skid
  buffer is (it lets you register the ready signal).
- **FSM vs pipeline:** FSMs sequence operations; pipelines overlap them.
  This design has both.
- **Reset:** control state uses an asynchronous active-low reset (`negedge rst_n`);
  datapaths have no reset so they map to LUTRAM, SRLs and DSP registers.
- **Resource trade-offs:** DSP48 blocks, block RAM, LUTRAM, and SRL shift
  registers. Know why a FIFO with a reset may not map to LUTRAM.
- **Fixed point:** Qm.n notation, and why the hardware uses integers.
  Rounding (add half, then shift), saturation vs wrap, and sign extension.
- **Division in hardware:** a restoring divider produces one quotient bit per
  step. Pipelining it gives one result per cycle; iterating costs little area
  but gives one result every N cycles. A reciprocal plus multiplies replaces
  several divides.

**AXI**
- **AXI4-Lite:** five channels (AW, W, B, AR, R), each with its own
  handshake. A write response must come after both the address and data
  handshakes. WSTRB selects bytes. OKAY / SLVERR / DECERR responses.
- **AXI4 bursts:** ARLEN is beats − 1, ARSIZE is the beat size, INCR bursts,
  and bursts must not cross 4 KB. RLAST marks the final beat. Multiple reads
  can be outstanding; with a single ID they return in order.

**Graphics**
- **The pipeline:** model → world → view → clip space (the MVP matrix), then
  the perspective divide to NDC, then the viewport transform to pixels.
- **Homogeneous w:** the divide by w is what makes far things smaller.
  w ≤ 0 means the point is at or behind the camera.
- **Clipping vs culling:** clipping cuts geometry at frustum planes (this
  design only rejects and drops). Back-face culling uses the sign of the
  signed area in screen space.
- **Edge functions (Pineda):** E(x, y) = (y − ay)·dx − (x − ax)·dy. A pixel is
  inside when all three are ≥ 0. Stepping one pixel adds a constant.
- **Fill rules:** top-left (Direct3D) or equivalent. They exist so shared
  edges draw exactly once.
- **Z-buffering:** keep the nearest depth per pixel, with a less-than test.
  z/w is linear in screen space, so it interpolates correctly.
- **Gouraud shading:** interpolate vertex colours. It's not
  perspective-correct; perspective-correct interpolation divides attribute/w
  by 1/w per pixel.
- **Sub-pixel precision:** 4 fraction bits stop edges wobbling as
  triangles move.

**Verification**
- Golden or reference models, scoreboards, constrained-random stimulus,
  seeds, functional vs line coverage, and assertions/protocol checkers.
- Why line coverage isn't enough: code can run without its outputs being
  checked. That's why every test compares against the model.

---

## 8. Limitations and next steps

State these confidently. Knowing your design's limits is a strength.

| Limitation | Why it's acceptable now | What I'd do next |
|---|---|---|
| Not run on hardware | no board available | Zynq block design: PS writes registers over AXI GP, vertices in DDR over HP |
| Triangles crossing the near plane are dropped, not clipped | the demo keeps geometry in front of the camera | homogeneous near-plane clipper before the divide (one triangle can become two) |
| Colour isn't perspective-correct | Gouraud is the classic trade-off; depth *is* correct | interpolate attribute/w and 1/w, divide per pixel |
| The clear costs 76,801 cycles, most of a simple frame | simple and correct | fast clear: per-tile "cleared" bits, or clear multiple pixels per write |
| Uses 98 BRAM tiles vs a 76 minimum | still fits (70%) | split each buffer into 64K + 12K parts |
| 100 MHz timing margin is small (+0.253 ns) | meets the target | fold "+2^20" into the previous stage; register vertex fetch's burst length |
| 320x240, 16-bit colour | fits on-chip BRAM | external DDR framebuffer with a write-combining cache |
| One ROP, 1 pixel per cycle | the depth RAM has one read-modify-write port | tile-based or multi-bank ROP |

---

## 9. Question bank

Practise answering out loud in under a minute each.

1. **"Walk me through what happens to one triangle."** Fetch (burst, FIFO),
   transform (4 rows), divide and viewport (flags), assembly, setup (reject,
   cull, edges, gradients), rasterize (spans, add-only), ROP (read, forward,
   compare, write), DONE.
2. **"What was the hardest bug?"** Pick one: the ROP hazard (design), the
   timing failure (implementation), or cull configuration read too late
   (found by random testing).
3. **"How do you know it's correct?"** Two independent formulations agree
   bit-exactly; random plus directed tests; 98.9% coverage with each miss
   explained; protocol checkers.
4. **"What limits performance?"** For simple scenes, the clear (77k of about
   100k cycles). Then the ROP's 1 fragment per cycle. Vertex fetch is capped
   at 4 cycles per vertex by the 32-bit bus.
5. **"How would you double throughput?"**
   - Fast clear first; it's the biggest win for simple scenes.
   - Then 2 ROPs with the framebuffer interleaved by pixel, so each handles
     half the pixels.
   - Then a 64-bit fetch bus.
6. **"Why is the depth test pipelined with forwarding instead of stalling?"**
   See story 2.
7. **"Why 4 multipliers in the transform?"** See story 3.
8. **"What's WNS? How did you fix timing?"** See story 1.
9. **"What happens if a vertex is behind the camera?"** w ≤ 1/16 sets a NEAR
   flag, and setup drops the whole triangle and counts it. It isn't clipped;
   that's a known limitation.
10. **"How does the rasterizer decide a pixel is inside?"** Three edge
    functions, all ≥ 0 after winding normalisation, with a top-left bias.
11. **"Why fixed point, not floating point?"** It's cheaper in LUTs and DSPs,
    and it's deterministic, so it can be bit-matched against a model. The
    formats are sized to what's needed (table in section 3).
12. **"What would change for an ASIC?"**
    - SRAM macros instead of block RAM.
    - A real clock and reset strategy.
    - Scan/DFT insertion.
    - Clock gating for the idle stages.
    - Formal verification of the handshakes.
    - Tighter area: the DSP-sized multipliers become custom datapaths.
13. **"How would you verify this formally?"**
    - Assert valid/ready rules: valid stays stable until ready, data holds.
    - Prove FIFO no-overflow from the credit counter.
    - Prove the ROP forwarding matches a non-pipelined reference, e.g. with
      SymbiYosys.
14. **"What's AXI's 4 KB rule and how do you handle it?"** A burst can't cross
    a 4 KB boundary. The burst length is min(4, remaining vertices, vertices
    left before the boundary); a directed test checks the split.
15. **"What does DONE mean?"** Fetch has finished and every stage reports
    idle, including the last pending RAM write, so it's safe to read the
    frame.
16. **"Why is your BRAM count 98, not 76?"** A 76,800-deep memory isn't a
    power of two; Vivado's default mapping wastes part of the depth. The fix
    is a manual split.
17. **"What did Yosys vs Vivado teach you?"** Different tools map
    differently; don't mix their numbers. And the `ltp` pitfall.
18. **"What would you do with another month?"** Near-plane clipping, fast
    clear, a Zynq board bring-up, and perspective-correct colour.

---

## 10. Company-specific angles

- **NVIDIA / AMD (GPU):**
  - Lead with rasterizer correctness (fill rule, interpolation) and the ROP
    forwarding hazard.
  - Know how real GPUs differ:
    - tiled/binned rasterization;
    - hierarchical Z;
    - many ROPs and shader cores;
    - fixed-function vs programmable stages.
  - Be ready to discuss throughput ("fragments/cycle") and memory bandwidth.
- **Qualcomm (mobile / Adreno):**
  - Emphasise power and area thinking: rate matching, DSP savings, SRL/LUTRAM
    mapping.
  - Clear cost and bandwidth matter on mobile. Mention tile-based rendering,
    which keeps depth on chip, like this design, to save DRAM bandwidth.
- **Verification roles anywhere:**
  - Golden model, randomisation with seeds, protocol checkers.
  - Coverage with justified exclusions.
  - The test-found bugs (section 5).
- **FPGA roles:**
  - The timing closure story.
  - BRAM mapping inefficiency.
  - Out-of-context implementation.
  - Resources as a percentage of the 7020.
