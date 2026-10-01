# UVM environment for the ROP

A SystemVerilog/UVM 1.2 testbench for [`rtl/rop.sv`](../../rtl/rop.sv), the
depth-test and framebuffer unit. It implements the features in
[`docs/VERIFICATION_PLAN.md`](../../docs/VERIFICATION_PLAN.md). It runs
alongside the Verilator/C++ regression, which is unchanged: the Makefile and
CI do not compile anything in this directory.

## Running it

It needs the Vivado simulator (`xsim`, included with the free Vivado ML
Standard edition). From Git Bash at the repository root:

```sh
tb/uvm/run_xsim.sh                         # rop_full_test, seed 1
tb/uvm/run_xsim.sh rop_hazard_test 7       # any test, any seed
```

Each run compiles into `build/uvm/<test>/` and prints the scoreboard summary,
the coverage per covergroup and the verdict; the full log is `run.log`.
`ROP_RTL=<path> tb/uvm/run_xsim.sh ...` runs against a different `rop.sv`
(used for the bug-injection check below).

The code is plain UVM 1.2 with no simulator-specific features, so it should
also run on other simulators (for example VCS on EDA Playground), but it has
only been run on xsim.

## Structure

```
tb_top.sv          clock, reset, DUT, interface; calls run_test()
rop_if.sv          signals + clocking blocks (drv_cb for driving, mon_cb for sampling)
rop_pkg.sv         package: constants, analysis-imp declarations, includes below
  rop_item.sv        rop_item (fragment or clear) and rop_decision (scoreboard -> coverage)
  rop_sequences.sv   clear, random, hazard storm, edges, clear+draw, directed forwarding
  rop_agent.sv       sequencer, driver, monitor, agent
  rop_scoreboard.sv  in-order reference model; frag_pass, clear and readback checks
  rop_coverage.sv    covergroups for F1, F2/F3, F5, F6
  rop_env.sv         agent + scoreboard + coverage, wired together
  rop_tests.sv       base test (reset, clear, body, drain, readback, verdict) + tests
run_xsim.sh        compile and run with xsim
```

Data flow:

```
sequence -> sequencer -> driver -> DUT pins <- monitor
                                                 |  frag_ap: accepted fragments
                                                 |  pass_ap: frag_pass pulses
                                                 |  clear_ap: completed clears
                                                 v
                                            scoreboard --decisions--> coverage
```

## Tests

| Test | Stimulus |
|---|---|
| `rop_fwd_directed_test` | directed forwarding sweep: distance 1-5 x (second farther / equal / closer) x (idle cycles / filler fragments) |
| `rop_random_test` | 3,000 uniformly random on-screen fragments with occasional idle gaps |
| `rop_hazard_test` | 5,000-fragment hazard storm on 4 pixels, depth biased to ties / closer / farther / extremes |
| `rop_gap_test` | hazard storm on 2 pixels with gaps of up to 8 idle cycles |
| `rop_clear_draw_test` | three rounds of clear (black, white, random colour) followed by draw traffic |
| `rop_full_test` | all of the above in one simulation, for the coverage numbers |

Every test starts with a clear (the RAMs power up unknown), and ends by
draining the pipeline and reading every pixel's colour and depth back
through the external port.

## Checks

- **frag_pass:** every fragment the model says should be written must
  produce one `frag_pass` pulse exactly 4 cycles after it was accepted.
  Missing, extra or late pulses are errors, and each error names the
  fragment and its distance to the previous write to the same pixel.
- **Readback:** at end of test all 76,800 colours and depths must match the
  model.
- **Clear:** `clear_busy` must stay high for exactly 76,800 cycles, and
  `in_ready` must be low throughout.
- **Handshake:** the number of fragments the driver sent must equal the
  number the monitor saw accepted.
- **Precondition P1:** the monitor flags any `start_clear` while the ROP is
  not idle (a check on our own stimulus; the assertions come later).

## Results

Vivado xsim 2026.1, UVM 1.2. Command: `tb/uvm/run_xsim.sh rop_full_test <seed>`.

| Seed | Fragments accepted | pass / fail_greater / fail_equal | frag_pass matched | Clears | Readback mismatches | Errors | Coverage (all 5 covergroups) |
|---|---|---|---|---|---|---|---|
| 1 | 12,890 | 3,433 / 7,941 / 1,516 | 3,433 / 3,433 | 4 | 0 of 153,600 | 0 | 100% |
| 2 | 12,890 | 3,404 / 7,886 / 1,600 | 3,404 / 3,404 | 4 | 0 of 153,600 | 0 | 100% |

Each run simulates about 4.81 ms (481k cycles at 100 MHz) and took about
5.5 minutes of wall-clock time. Coverage is printed by the coverage
component with `get_inst_coverage()`, since the free Vivado licence does not
include the `xcrg` coverage report tool. It is per run, not merged across
runs, which is why `rop_full_test` runs every sequence in one simulation.

## Bug injection

To check that the scoreboard catches a forwarding bug, line 98 of a copy of
`rop.sv` was changed to skip history entry 1, the write made two cycles
earlier, so that one is never forwarded:

```diff
-            if (w_en[h] && w_addr[h] == a3) z_old = w_z[h];
+            if (h != 1 && w_en[h] && w_addr[h] == a3) z_old = w_z[h];
```

| Command | Result |
|---|---|
| `ROP_RTL=<mutant> tb/uvm/run_xsim.sh rop_fwd_directed_test 1` | TEST FAILED, 10 errors: 4 unexpected `frag_pass` pulses + 6 readback mismatches |
| `ROP_RTL=<mutant> tb/uvm/run_xsim.sh rop_full_test 1` | TEST FAILED, 13 errors: 12 `frag_pass` errors + 1 readback mismatch |
| `tb/uvm/run_xsim.sh rop_fwd_directed_test 1` (unmodified RTL) | TEST PASSED, 0 errors |

Every `frag_pass` error in the directed run names the cause, for example:

```
[FRAG_PASS] unexpected frag_pass pulse at cycle 76823: fragment (16,102) z=07d0
accepted at cycle 76819, model says FAIL_GREATER, distance to last write 2, 1 in-flight matches
```

The fragment is farther than the one written two cycles earlier, so it
should fail. Without distance-2 forwarding the ROP compares against the
stale depth in the RAM, passes it, and overwrites the closer pixel.

To reproduce: copy `rtl/rop.sv` somewhere outside the repo, make the edit
above, and point `ROP_RTL` at the copy.
