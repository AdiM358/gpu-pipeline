# ROP Verification Plan

## Scope
`rtl/rop.sv` (depth test, forwarding, clear engine, colour and depth
buffers, external read port) is verified on its own, at its ports, with the
UVM 1.2 environment in [`tb/uvm/`](../tb/uvm/README.md), run on Vivado xsim.
Default parameters: 320 x 240 screen, 76,800 pixels. The rest of the GPU is
covered by the Verilator regression and is out of scope here.

## Features

### F1. Depth test (strict less-than)
- **Requirement:** A fragment is written if and only if its depth is strictly
  less than the depth currently stored at its pixel (`z_new < z_stored`). On a
  write, both the depth and the RGB565 colour at that pixel are updated, and
  `frag_pass` pulses once, one cycle after the decision. A fragment with equal
  or greater depth leaves the pixel unchanged and does not pulse `frag_pass`.
  Ties therefore keep the first fragment drawn.
- **Stimulus:** Constrained-random fragments aimed at a small pool of pixels
  (about 1–8), so most fragments land on a pixel that has been hit before.
  Depth is chosen relative to the stored value to give roughly equal shares of
  less-than, equal and greater-than, plus a knob forcing z = 0 and z = 0xFFFF.
  Include pixels still holding the clear value (depth 0xFFFF) as well as
  pixels that have already been drawn.
- **Checker:** The scoreboard keeps a reference depth and colour buffer,
  applies the same strict-less-than rule in arrival order, and predicts each
  fragment's result. (1) Every fragment the model says should write must
  produce exactly one `frag_pass` pulse, exactly 4 cycles after it was
  accepted; a missing, extra or mistimed pulse is an error.
  (2) At end of test, both buffers are read back through `ext_addr` and every
  pixel is compared with the model.
- **Coverage:** covergroup `cg_depth_test`, sampled once per fragment at the
  decision:
  - `cp_outcome`: {pass, fail_greater, fail_equal}
  - `cp_z`: {z_zero = 0, z_max = 0xFFFF, z_mid = all other values}
  - `cp_pixel_state`: {cleared (never drawn since the last clear), drawn}
  - `cx_outcome_z`: `cp_outcome` x `cp_z`, excluding the two impossible pairs
    `pass` x `z_max` (a z = 0xFFFF fragment can never be strictly less than a
    16-bit stored depth) and `fail_greater` x `z_zero` (z = 0 can never be
    greater than the stored depth)
  - **Goal:** 100% of bins in every coverpoint and cross.
- **Status:** implemented; 100% in `rop_full_test`, seeds 1 and 2.

### F2. Read-after-write forwarding
- **Requirement:** A fragment must be compared against the most recent depth
  *written* to its pixel, including writes that have been decided but have
  not yet reached the RAM. A write reaches the RAM 4 cycles after its fragment
  was accepted, so a later fragment to the same pixel accepted 1, 2 or 3
  cycles after it must use the forwarded value from the write history
  (`w_*[0..2]`); at 4 or more cycles the value is already in the RAM. If more
  than one in-flight write matches, the newest must be used. Fragments that
  failed the depth test write nothing and must not be forwarded.
  *Distance* is measured in clock cycles between the two fragments being
  accepted (`in_valid && in_ready`), not in fragments, because the history
  shifts every cycle.
- **Stimulus:**
  - *Directed sweep (sanity):* on a cleared pixel, send a fragment with depth
    A, then a second fragment to the same pixel at distance d = 1, 2, 3, 4, 5.
    The gap is filled either with fragments to *other* pixels or with idle
    cycles (`in_valid` = 0). For each d, run three orders: second farther
    (A < B), equal (A = B) and second closer (A > B). The first two are the
    ones that expose a missing forward: against stale RAM data (0xFFFF) the
    second fragment would wrongly pass.
  - *Random hazard storm (coverage closure):* back-to-back fragments over a
    pool of 1–4 pixels, random idle gaps of 0–4 cycles, and depth biased
    toward less / equal / greater relative to the pixel's current depth,
    including runs of decreasing depths so that two or three in-flight writes
    match at once.
- **Checker:** No new checker. F1's reference model applies fragments in
  order with no pipeline, so it always sees the true newest depth; it is the
  forwarding oracle. A stale or wrong forward makes the RTL's decision differ
  from the model, which shows up as a `frag_pass` mismatch and in the
  end-of-test framebuffer readback.
- **Coverage:** covergroup `cg_forwarding`, sampled once per fragment at the
  decision:
  - `cp_distance`: cycles since the most recent write to the same pixel:
    {d1, d2, d3, d4, far (5 or more, or no earlier write)}. d4 is the first
    distance served by the RAM instead of forwarding, the boundary most likely
    to hide an off-by-one.
  - `cp_matches`: {none, one, multiple}: how many in-flight writes (last 3
    cycles) matched the fragment's pixel.
  - `cp_gap`: {back_to_back, with_idle}: whether any `in_valid` = 0
    cycles fell between the two fragments (links to F3).
  - `cx_distance_outcome`: `cp_distance` x F1's `cp_outcome` (15 bins, none
    impossible).
  - **Goal:** 100% of bins in every coverpoint and cross.
- **Bug injection:** disabling the distance-2 forward (`rop.sv` line 98,
  `h != 1 &&`) fails `rop_fwd_directed_test` with 10 errors and
  `rop_full_test` with 13; see [`tb/uvm/README.md`](../tb/uvm/README.md).
- **Status:** implemented; 100% in `rop_full_test`, seeds 1 and 2.

### F3. Bubbles (valid gaps)
- **Requirement:** Idle cycles (`in_valid` = 0) between fragments must not
  change any result. The pipeline valid bits (`v1..v3`) and the write history
  shift every cycle whether or not a fragment was accepted, so a bubble
  must age an in-flight write exactly like a fragment does: a write followed
  by two idle cycles and then a fragment to the same pixel is distance 3, not
  distance 1. A bubble must never produce a write or a `frag_pass` pulse.
- **Stimulus:** The driver inserts `gap` idle cycles before a fragment
  (`rop_item.gap`, 0–16). The base sequences give 25% of fragments a gap of
  1..`max_gap`; `rop_gap_test` uses gaps of up to 8 over a 2-pixel pool, so
  pairs to the same pixel land on both sides of the d3/d4 boundary through
  idle cycles. The F2 directed sweep repeats every distance with idle fill
  instead of fragment fill.
- **Checker:** F1's model. It ignores time entirely, so any bubble that
  changes the RTL's decision is a mismatch. The F2 coverage counts distance
  in cycles from the monitor's cycle counter, so idle cycles are included.
- **Coverage:** `cg_forwarding.cp_gap` {back_to_back, with_idle}, only
  sampled when the previous write is at distance 1–4 (`iff (d != D_FAR)`).
  - **Goal:** 100%.
  - **Known gap:** `cp_gap` is not crossed with `cp_distance`, so the
    plan does not prove that every distance 2–4 was reached through idle
    cycles (the directed sweep does this, but coverage does not record it).
- **Status:** implemented; 100% in `rop_full_test`, seeds 1 and 2.

### F4. Input handshake
- **Requirement:** A fragment is accepted on a rising edge when `in_valid`
  and `in_ready` are both high, and only then. `in_ready` is low exactly while
  `clear_busy` is high (`rop.sv` line 58), and high otherwise: the ROP never
  stalls fragments for any other reason. Every fragment the sender presents is
  accepted exactly once: none lost, none duplicated.
- **Stimulus:** All sequences. The driver holds a fragment with `in_valid`
  high until it sees `in_ready`, then drops `in_valid` unless the next item
  follows immediately (back-to-back).
- **Checker:**
  - Monitor: error if `in_ready` and `clear_busy` are both high in any
    cycle.
  - Test `check_phase`: the number of fragments the driver sent must equal
    the number the scoreboard saw accepted.
- **Coverage:** none dedicated; back-to-back accepts are covered by
  `cp_gap.back_to_back`.
  - **Known gap:** the driver never presents a fragment while a clear is
    running, because a clear blocks the sequencer until `clear_busy` falls.
    So a fragment held with `in_valid` high against `in_ready` low is never
    exercised. A sequence that runs draws in parallel with a clear would close
    this.
- **Status:** implemented (checks pass in all runs); back-pressure not
  exercised.

### F5. Clear
- **Requirement:** A `start_clear` pulse while idle sets `clear_busy`, which
  stays high for exactly `NPIX` (76,800) cycles while the clear engine writes
  depth 0xFFFF and `clear_color` to one pixel per cycle (`rop.sv` lines
  120–130, 143–146). After it falls, every pixel holds depth 0xFFFF and the
  clear colour. Clear writes go through the same write history as fragment
  writes, so a fragment accepted in the first cycles after `clear_busy`
  falls must see the cleared depth, not the depth stored before the clear.
- **Stimulus:** Every test starts with a clear with a random colour (the RAMs
  power up unknown). `rop_clear_draw_seq` runs three rounds of clear (black,
  white, random colour), each followed directly by 400 hazard-storm
  fragments, so the first fragments arrive right after the clear ends and
  pixels drawn before the clear are hit again.
- **Checker:**
  - Monitor counts `clear_busy` cycles; the scoreboard checks the count
    equals `NPIX` and that no `frag_pass` pulses are outstanding.
  - The scoreboard resets its model to 0xFFFF and the clear colour, so the
    F1 checks on later fragments, and the final readback (undrawn pixels
    must hold the last clear colour), check the clear result.
- **Coverage:**
  - `cg_clear`: `cp_color` {black, white, other} and `cp_after_traffic`
    {first_clear, after_drawing}.
  - `cg_after_clear`: `cp_since_clear` {immediate (1–4 cycles after the
    clear ended), soon (5–16), later (17+)}, sampled per fragment.
  - **Goal:** 100%.
- **Status:** implemented; 100% in `rop_full_test`, seeds 1 and 2.

### F6. Screen edges and corners
- **Requirement:** Pixel (x, y) maps to address `y * SCREEN_W + x`
  (`rop.sv` line 135). Every on-screen pixel, including column 0, column
  `SCREEN_W - 1`, row 0, row `SCREEN_H - 1` and the four corners, has its own
  address: no two pixels alias, and none is lost.
- **Stimulus:** `rop_edge_seq`: 600 fragments with x picked from {0,
  `SCREEN_W - 1`, random interior} and y likewise, random depth.
- **Checker:** F1's model plus the full readback: a wrong address writes the
  wrong pixel, which shows up as two readback mismatches.
- **Coverage:** `cg_edges`: `cp_x` {left, right, mid} x `cp_y` {top, bottom,
  mid}, crossed into 9 bins (4 corners, 4 edges, interior).
  - **Goal:** 100%.
- **Status:** implemented; 100% in `rop_full_test`, seeds 1 and 2.

### F7. External read port
- **Requirement:** `ext_data` returns the value at `ext_addr` 2 cycles after
  the address is presented: the colour buffer when `ext_depth` was 0, the
  depth buffer when it was 1 (`ext_depth` is delayed to match, `rop.sv` line
  91). Colour reads work at any time. Depth reads share the Z RAM read port
  with the depth test (line 80) and are only valid while idle (P3).
- **Stimulus:** At the end of every test, after draining the pipeline, the
  test reads all 76,800 colours and then all 76,800 depths, one address per
  cycle.
- **Checker:** each read is compared with the model, offset by the 2-cycle
  latency (153,600 values per test).
- **Coverage:** none dedicated; every address is read in both modes in every
  test.
  - **Known gaps:** colour reads during drawing are not tested, and
    `ext_depth` only changes once per test, so a back-to-back
    colour/depth switch is not tested.
- **Status:** implemented; 0 mismatches in all passing runs.

## Preconditions (assumed by the ROP; asserted in step 3)
- **P1. `start_clear` only while `idle`.** If a clear starts while fragments
  are still in R1–R3, they still raise `frag_pass` (line 115 is outside the
  `if (clear_busy)` branch), but their writes are replaced by clear writes
  (line 125 is not reached). `frag_pass` would then count writes that never
  happened. The driver waits for `idle` before a clear and the monitor
  flags any violation of our own stimulus.
- **P2. Fragments are on screen:** `in_x < SCREEN_W` and `in_y <
  SCREEN_H`. The ROP does not check bounds: an off-screen fragment aliases to
  another pixel (x = 320, y = 0 writes pixel (0, 1)) or past the end of the
  buffers. Enforced by the `c_on_screen` constraint in `rop_item`.
- **P3. Depth reads on the external port only while `idle`.** The Z RAM
  read address is `v1 ? a1 : ext_addr` (line 80), so a fragment in R1 takes
  the port and the external read returns that fragment's old depth instead.
  The test only reads depth after `drain()` has waited for `idle`.

## Results

Command: `tb/uvm/run_xsim.sh rop_full_test <seed>` (Vivado xsim 2026.1).

| Seed | Fragments | `frag_pass` matched | Readback mismatches | Errors | Coverage |
|---|---|---|---|---|---|
| 1 | 12,890 | 3,433 / 3,433 | 0 of 153,600 | 0 | 100% on all 5 covergroups |
| 2 | 12,890 | 3,404 / 3,404 | 0 of 153,600 | 0 | 100% on all 5 covergroups |
