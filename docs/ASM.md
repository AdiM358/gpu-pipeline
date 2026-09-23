# RTL ASM charts

Algorithmic State Machine (ASM) charts for every controller in the pipeline,
drawn from the RTL in `rtl/`. They complement
[ARCHITECTURE.md](ARCHITECTURE.md), which shows *what* each stage does. These
charts show *how*: which registers change, on which condition, in which cycle.

## Notation

| Symbol | Meaning |
|---|---|
| **Rectangle** (state box) | A state. The first line is the state name; the rest are Moore outputs (true for the whole state) and unconditional register transfers `r ← expr`. |
| **Diamond** (decision box) | A condition evaluated in the same clock cycle. |
| **Rounded box** (conditional output) | A Mealy output or register transfer that happens only on that path, in the same clock cycle. |
| Arrow into a state box | The next state, taken at the next rising clock edge. |

A state box plus all the decisions and conditional boxes below it, up to the
next state box, is one **ASM block**: everything in it happens in one clock
cycle. Transfers use non-blocking semantics, so every `←` in a block reads
the values from *before* the edge. Single-cycle pulses (`start_draw`,
`culled`, `error_set`, ...) default to 0 in every state unless a box sets
them. `en` in a pipelined stage means "the downstream register is free":
`en = not out_valid or out_ready`.

---

## 0. Controller / datapath overview (ASMD)

The whole GPU is one control unit, `gpu_ctrl` (sequenced by software through
`axil_regs`), driving a chain of datapath stages. The stages hand data to each
other with valid/ready handshakes and report status back.

```mermaid
flowchart LR
    SW["CPU driver"] -- "AXI4-Lite" --> REGS["axil_regs<br/>register file"]
    REGS -- "cmd_draw, cmd_clear" --> CTRL["gpu_ctrl<br/>command ASM (section 1)"]
    CTRL -- "busy, done_set" --> REGS
    CTRL -- "start_draw" --> VF
    CTRL -- "start_draw = flush" --> PA
    CTRL -- "start_clear" --> ROP
    REGS -. "MVP, base, count, cull cfg, clear colour" .-> VF
    subgraph DP ["Datapath (valid/ready between every stage)"]
        VF["vertex_fetch<br/>(section 3)"] --> GE["geom_engine<br/>(section 4)"]
        GE --> PV["persp_viewport<br/>(section 5)"]
        PV --> PA["prim_assembly<br/>(section 6)"]
        PA --> TS["tri_setup<br/>(section 7)"]
        TS --> RA["rasterizer<br/>(section 8)"]
        RA --> ROP["rop<br/>(section 9)"]
    end
    VF -- "fetch_busy, error_set" --> CTRL
    ROP -- "clear_busy" --> CTRL
    DP -- "pipe_idle = AND of every stage's idle" --> CTRL
```

---

## 1. `gpu_ctrl`: command sequencer

One command is CLEAR, DRAW, or both (clear first). DONE is signalled only when
the vertex fetch has finished **and** every stage has been idle for
`DRAIN_CYCLES + 1` consecutive cycles (`gpu_top` uses `DRAIN_CYCLES = 1`), so
DONE means every pixel is in memory.

```mermaid
flowchart TD
    IDLE["<b>S_IDLE</b><br/>busy = 0"] --> D1{"cmd_draw or cmd_clear?"}
    D1 -- no --> IDLE
    D1 -- yes --> C1(["cmd_start ← 1 (clears perf counters)<br/>draw_pending ← cmd_draw"])
    C1 --> D2{"cmd_clear?"}
    D2 -- yes --> C2(["start_clear ← 1"])
    C2 --> CLR["<b>S_CLEAR</b><br/>busy = 1<br/>(one cycle for clear_busy to rise)"]
    D2 -- no --> C3(["start_draw ← 1"])
    C3 --> DRAW
    CLR --> CW["<b>S_CLEAR_WAIT</b><br/>busy = 1"]
    CW --> D3{"clear_busy?"}
    D3 -- yes --> CW
    D3 -- no --> D4{"draw_pending?"}
    D4 -- yes --> C4(["start_draw ← 1"])
    C4 --> DRAW["<b>S_DRAW</b><br/>busy = 1<br/>idle_run ← 0"]
    D4 -- no --> C5(["done_set ← 1"])
    C5 --> IDLE
    DRAW --> D5{"start_draw = 0 and<br/>fetch_busy = 0?"}
    D5 -- no --> DRAW
    D5 -- yes --> DRAIN["<b>S_DRAIN</b><br/>busy = 1"]
    DRAIN --> D6{"pipe_idle?"}
    D6 -- no --> C6(["idle_run ← 0"])
    C6 --> DRAIN
    D6 -- yes --> D7{"idle_run = DRAIN_CYCLES?"}
    D7 -- no --> C7(["idle_run ← idle_run + 1"])
    C7 --> DRAIN
    D7 -- yes --> C8(["done_set ← 1 (sets STATUS.DONE)"])
    C8 --> IDLE
```

Commands that arrive while `busy = 1` are ignored: only `S_IDLE` looks at them.

---

## 2. `axil_regs`: AXI4-Lite slave

### Write channel

The write channel's state is the three flags `aw_held`, `w_held` and
`s_axi_bvalid`. AW and W are captured independently, in any order or in
the same cycle, so the chart is one ASM block with parallel decisions,
evaluated every cycle.

```mermaid
flowchart TD
    W0["<b>WRITE CHANNEL</b> (every cycle)<br/>awready = not aw_held<br/>wready = not w_held<br/>cmd_draw ← 0, cmd_clear ← 0"] --> A{"awvalid and awready?"}
    A -- yes --> A1(["aw_held ← 1<br/>aw_word ← awaddr[7:2]"])
    A -- no --> WV
    A1 --> WV{"wvalid and wready?"}
    WV -- yes --> W1(["w_held ← 1<br/>w_data ← wdata, w_strb ← wstrb"])
    WV -- no --> BV
    W1 --> BV{"bvalid and bready?"}
    BV -- yes --> B1(["bvalid ← 0"])
    BV -- no --> DW
    B1 --> DW{"aw_held and w_held<br/>and not bvalid?"}
    DW -- no --> EV
    DW -- yes --> WR(["aw_held ← 0, w_held ← 0<br/>bvalid ← 1<br/>bresp ← OKAY if mapped, else SLVERR<br/>reg[aw_word] ← byte-merge(w_data, w_strb)"])
    WR --> K{"aw_word = CTRL<br/>and w_strb[0]?"}
    K -- yes --> K1(["cmd_draw ← w_data[0]<br/>cmd_clear ← w_data[1]<br/>if a command and not busy: done_q ← 0"])
    K -- no --> S{"aw_word = STATUS<br/>and w_strb[0]?"}
    S -- yes --> S1(["write-1-to-clear:<br/>done_q ← 0 if w_data[1]<br/>err_q ← 0 if w_data[2]"])
    S -- no --> EV
    K1 --> EV
    S1 --> EV{"done_set / err_set?"}
    EV -- yes --> E1(["done_q ← 1 / err_q ← 1<br/>(overrides a same-cycle clear)"])
    EV -- no --> W0
    E1 --> W0
```

### Read channel

```mermaid
flowchart TD
    R0["<b>R_IDLE</b><br/>arready = 1, rvalid = 0"] --> RA{"arvalid?"}
    RA -- no --> R0
    RA -- yes --> RA1(["rdata ← mux(araddr): regs, STATUS, ID,<br/>FB_INFO, perf[i], MVP[r][c]<br/>rresp ← OKAY if mapped, else SLVERR"])
    RA1 --> R1["<b>R_VALID</b><br/>arready = 0, rvalid = 1"]
    R1 --> RR{"rready?"}
    RR -- no --> R1
    RR -- yes --> R0
```

---

## 3. `vertex_fetch`: burst reader with prefetch

`IDLE` and `ACTIVE` are the `active` flag. In `ACTIVE`, four activities run
in the **same** clock cycle: issue a read request, receive a beat, output a
vertex, and check for completion. They are drawn as one ASM block.

```mermaid
flowchart TD
    I0["<b>IDLE</b><br/>busy = 0, rready = 1"] --> ST{"start?"}
    ST -- no --> I0
    ST -- yes --> CNT{"vertex_count = 0?"}
    CNT -- yes --> I0
    CNT -- no --> INIT(["active ← 1, err ← 0<br/>req_addr ← base with bits 3:0 cleared<br/>req_left ← vertex_count<br/>word_idx ← 0, beat_idx ← 0"])
    INIT --> ACT

    ACT["<b>ACTIVE</b><br/>busy = 1, rready = 1<br/>n = min(4, req_left, vertices to next 4 KB)"] --> ARH{"arvalid and arready?"}
    ARH -- yes --> ARH1(["arvalid ← 0"])
    ARH -- no --> ISS
    ARH1 --> ISS{"can_issue?<br/>not err, req_left ≠ 0,<br/>FIFO count + in flight + n ≤ 16,<br/>bursts_out below 4, AR slot free"}
    ISS -- yes --> ISS1(["arvalid ← 1, araddr ← req_addr<br/>arlen ← 4n − 1, len_q.push(n)<br/>req_addr ← req_addr + 16n<br/>req_left ← req_left − n<br/>inflight += n, bursts_out += 1"])
    ISS -- no --> RV
    ISS1 --> RV{"rvalid?"}
    RV -- no --> OUT
    RV -- yes --> BAD{"rresp ≠ OKAY or<br/>rlast ≠ (last beat of burst)?"}
    BAD -- yes --> BAD1(["err ← 1<br/>error_set ← 1 (first error only)"])
    BAD -- no --> WD
    BAD1 --> WD{"word_idx = 3?"}
    WD -- yes --> PUSH(["FIFO.push(x, y, z, rdata)<br/>unless err or this beat is bad<br/>inflight −= 1"])
    WD -- no --> WS(["word[word_idx] ← rdata"])
    PUSH --> LB
    WS --> LB{"last beat of burst?"}
    LB -- yes --> LB1(["len_q.pop, bursts_out −= 1<br/>beat_idx ← 0"])
    LB -- no --> LB2(["beat_idx += 1"])
    LB1 --> OUT
    LB2 --> OUT{"FIFO not empty and<br/>output free and not err?"}
    OUT -- yes --> OUT1(["out_x, out_y, out_z, out_color ← FIFO.pop<br/>out_valid ← 1"])
    OUT -- no --> ER
    OUT1 --> ER{"err?"}
    ER -- yes --> ER1(["flush FIFO, out_valid ← 0"])
    ER -- no --> DN
    ER1 --> DN{"bursts_out = 0, no AR pending, and<br/>(err or (req_left = 0 and FIFO empty<br/>and not out_valid))?"}
    DN -- yes --> DN1(["active ← 0"])
    DN1 --> I0
    DN -- no --> ACT
```

---

## 4. `geom_engine`: rate-matched transform

### Issue controller

Each vertex needs four row issues. `busy` and `row` are the state. All
transfers happen only when `en = 1`; with `en = 0` the whole unit holds.

```mermaid
flowchart TD
    G0["<b>G_IDLE</b><br/>busy = 0<br/>in_ready = en"] --> GE0{"en?"}
    GE0 -- no --> G0
    GE0 -- yes --> GV0{"in_valid?"}
    GV0 -- no --> G0
    GV0 -- yes --> GL0(["v ← (x, y, z, 1.0), color_i ← color<br/>row ← 0, busy ← 1"])
    GL0 --> G1
    G1["<b>G_ISSUE(row)</b><br/>busy = 1<br/>in_ready = en and row = 3<br/>(if en) a_i[c] ← M[row][c], b_i[c] ← v[c], vld_i ← 1"] --> GE1{"en?"}
    GE1 -- no --> G1
    GE1 -- yes --> GR{"row = 3?"}
    GR -- no --> GRI(["row ← row + 1"])
    GRI --> G1
    GR -- yes --> GV1{"in_valid?"}
    GV1 -- yes --> GL1(["next vertex: v ← (x, y, z, 1.0)<br/>row ← 0 (no bubble)"])
    GL1 --> G1
    GV1 -- no --> GB(["busy ← 0"])
    GB --> G0
```

### Datapath pipeline (register transfers per stage, all gated by `en`)

```mermaid
flowchart LR
    P0["<b>I</b><br/>a_i[c] ← M[row][c]<br/>b_i[c] ← v[c]"] --> P1["<b>P1</b><br/>p1[c] ← (a_i[c] × b_i[c])[47:0]"]
    P1 --> P2["<b>P2</b><br/>p2[c] ← p1[c]"]
    P2 --> P3["<b>S</b><br/>s01 ← p2[0] + p2[1]<br/>s23 ← p2[2] + p2[3]"]
    P3 --> P4{"<b>F</b>: row = 3?"}
    P4 -- "row 0..2" --> P5(["acc[row] ← (s01 + s23)[47:16]"])
    P4 -- "row 3" --> P6(["out_x,y,z ← acc[0..2]<br/>out_w ← (s01 + s23)[47:16]<br/>out_valid ← 1"])
```

---

## 5. `persp_viewport` and `recip_pipe`: pure pipeline

There is no FSM: every stage moves one step whenever `en = 1`. Each box is
one pipeline register stage and its register transfers.

```mermaid
flowchart LR
    F["<b>F</b><br/>fx,fy,fz,fw,fcol ← inputs<br/>vld_f ← in_valid"] --> F2["flags (combinational):<br/>oc[5:0] = x,y,z vs ±w (33-bit)<br/>near = (w ≤ 1/16)<br/>d = 0x2000 if near, else w"]
    F2 --> D["<b>D0..D31</b> (recip_pipe)<br/>32 stages, see below<br/>payload {near, oc, col, z, y, x} rides along"]
    D --> M1["<b>M1</b><br/>px1 ← x × recip<br/>py1 ← y × recip<br/>pz1 ← z × recip"]
    M1 --> M2["<b>M2</b><br/>px2, py2, pz2 ← px1, py1, pz1"]
    M2 --> V["<b>V</b><br/>n = p >>> 24 (Q.20 NDC)<br/>sx ← (n_x·8W + 8W·2^20) >>> 20<br/>sy ← (8H·2^20 − n_y·8H) >>> 20<br/>z ← clamp((n_z + 2^20)·65535 >>> 21)<br/>flags ← {gb, near, oc}<br/>out_valid ← 1"]
```

One stage `k` of `recip_pipe` (q = ⌊2⁴⁴ / d⌋, one quotient bit per stage,
initial `rem = 2^12`):

```mermaid
flowchart TD
    K["<b>STAGE k</b> (if en)<br/>shifted = rem_k × 2<br/>div_(k+1) ← div_k<br/>vld_(k+1) ← vld_k"] --> KG{"shifted ≥ div_k?"}
    KG -- yes --> KY(["rem_(k+1) ← shifted − div_k<br/>quo_(k+1) ← (quo_k × 2) + 1"])
    KG -- no --> KN(["rem_(k+1) ← shifted<br/>quo_(k+1) ← quo_k × 2"])
```

---

## 6. `prim_assembly`: triangle assembly

`count` is the state (vertices held). `flush`, pulsed at the start of every
draw, takes priority and discards a partial triangle. Separately, in every
state, `out_valid ← 0` when `out_valid and out_ready`.

```mermaid
flowchart TD
    Q0["<b>C0</b> (count = 0)<br/>in_ready = 1"] --> F0{"flush?"}
    F0 -- yes --> Q0
    F0 -- no --> V0{"in_valid?"}
    V0 -- no --> Q0
    V0 -- yes --> H0(["h[0] ← vertex"])
    H0 --> Q1["<b>C1</b> (count = 1)<br/>in_ready = 1"]
    Q1 --> F1{"flush?"}
    F1 -- yes --> Q0
    F1 -- no --> V1{"in_valid?"}
    V1 -- no --> Q1
    V1 -- yes --> H1(["h[1] ← vertex"])
    H1 --> Q2["<b>C2</b> (count = 2)<br/>in_ready = not out_valid or out_ready"]
    Q2 --> F2{"flush?"}
    F2 -- yes --> Q0
    F2 -- no --> V2{"in_valid and in_ready?"}
    V2 -- no --> Q2
    V2 -- yes --> T(["out[0..2] ← h[0], h[1], vertex<br/>out_valid ← 1"])
    T --> Q0
```

---

## 7. `tri_setup`: triangle setup

The main ASM. Two helper machines run concurrently inside `S_RUN`: the
iterative divider and the shared-multiplier pipeline. Each has its own chart
below.

```mermaid
flowchart TD
    S0["<b>S_IDLE</b><br/>in_ready = 1"] --> SV{"in_valid?"}
    SV -- no --> S0
    SV -- yes --> SL(["vx, vy, va (z, r, g, b), vf ← triangle<br/>cfg_cull ← cull_back, cfg_front_cw ← front_cw"])
    SL --> S1["<b>S_CLASS</b><br/>dx1 ← x1 − x0, dy1 ← y1 − y0<br/>dx2 ← x2 − x0, dy2 ← y2 − y0<br/>xmin, xmax, ymin, ymax ← min/max of vertices"]
    S1 --> RJ{"reject?<br/>all 3 outcodes share a plane,<br/>or any NEAR or guard-band flag"}
    RJ -- yes --> RJ1(["clipped ← 1"])
    RJ1 --> S0
    RJ -- no --> S2["<b>S_AREA</b><br/>prod_a ← dx1 × dy2<br/>prod_b ← dx2 × dy1"]
    S2 --> S3["<b>S_DECIDE</b><br/>area = prod_a − prod_b<br/>px0..px1, py0..py1 ← pixel box clamped to screen<br/>area_pos ← |area|"]
    S3 --> CU{"area = 0, or cfg_cull and<br/>not front-facing?"}
    CU -- yes --> CU1(["culled ← 1"])
    CU1 --> S0
    CU -- no --> EM{"box empty?"}
    EM -- yes --> EM1(["clipped ← 1"])
    EM1 --> S0
    EM -- no --> NEG{"area below 0?"}
    NEG -- yes --> SW(["swap v1 ↔ v2<br/>dx1 ↔ dx2, dy1 ↔ dy2"])
    NEG -- no --> S4
    SW --> S4["<b>S_PREP</b><br/>per edge e: dxe, dye ← b − a<br/>tl[e] ← (dye below 0) or (dye = 0 and dxe above 0)<br/>ecx, ecy ← first pixel centre − edge start<br/>per attribute: da1, da2, amin, amax<br/>a0 ← a_v0 × 2^20, dcx, dcy<br/>lz ← clz(area_pos), d_den ← area_pos × 2^lz<br/>d_rem ← 2^30, d_cnt ← 25, d_busy ← 1<br/>op ← 0, wb_count ← 0"]
    S4 --> S5["<b>S_RUN</b><br/>(divider and multiplier charts below)"]
    S5 --> WB{"wb_count = 38?"}
    WB -- no --> S5
    WB -- yes --> S6["<b>S_OUT</b>"]
    S6 --> OF{"out_valid = 0 or out_ready?"}
    OF -- no --> S6
    OF -- yes --> OUT(["out_e0 ← e0, out_ex ← −16·dye, out_ey ← 16·dxe<br/>out_a0 ← a0, out_ax ← 16·Gx, out_ay ← 16·Gy<br/>out_amin, out_amax, out box<br/>out_valid ← 1"])
    OUT --> S0
```

### Iterative reciprocal: R = ⌊2⁵⁵ / (area × 2^lz)⌋

```mermaid
flowchart TD
    DV["<b>DIV</b> (while d_busy)<br/>shift = d_rem × 2<br/>d_cnt ← d_cnt − 1"] --> DG{"shift ≥ d_den?"}
    DG -- yes --> DY(["d_rem ← shift − d_den<br/>d_q ← d_q × 2 + 1"])
    DG -- no --> DN(["d_rem ← shift<br/>d_q ← d_q × 2"])
    DY --> DC
    DN --> DC{"d_cnt = 1?"}
    DC -- yes --> DD(["d_busy ← 0<br/>(d_q valid after 25 iterations)"])
    DC -- no --> DV
```

### Shared multiplier: issue and write-back

38 operations are issued in order, one per cycle when their inputs are
ready. Results return 4 cycles later: operand register, product, product
register, shift, then write-back.

```mermaid
flowchart TD
    MI["<b>ISSUE</b> (S_RUN, every cycle)"] --> RDY{"op below 38 and ready?<br/>ops 0–21: always<br/>ops 22–29: wb_count ≥ 22 and not d_busy<br/>ops 30–37: wb_count ≥ 30"}
    RDY -- no --> MW
    RDY -- yes --> MX(["ma_r, mb_r ← operands(op)<br/>t1 ← op, v1 ← 1, op ← op + 1"])
    MX --> MW{"v4 (a result from 4 cycles ago)?"}
    MW -- no --> MI
    MW -- yes --> TY{"which op t4?"}
    TY -- "0–5: edges" --> WE(["term 0: e0[e] ← p<br/>term 1: e0[e] ← e0[e] − p − (tl[e] ? 0 : 1)"])
    TY -- "6–21: numerators" --> WN(["num[i][axis] ← p, then ← num − p"])
    TY -- "22–29: gradients" --> WG(["grad[i][axis] ← (num × R) >>> (35 − lz), 38-bit"])
    TY -- "30–37: start values" --> WA(["a0[i] ← a0[i] + grad × (dcx or dcy)"])
    WE --> WC(["wb_count ← wb_count + 1"])
    WN --> WC
    WG --> WC
    WA --> WC
    WC --> MI
```

---

## 8. `rasterizer`: span rasterizer

Two cooperating ASMs plus an output register. **T** (traversal) walks the
bounding box `SPAN` pixels at a time. **S** (serializer) emits the covered
pixels of one span, one per cycle. T keeps skipping empty spans while S is
busy. A covered span has to wait until S can take it (`s_free`).

Shared signals:
- `mask[k] = (t_x + k ≤ px1) and all three e_cur[e] + exk[e][k] ≥ 0`
- `o_free = not frag_valid or frag_ready`
- `s_emit = s_valid and o_free`
- `s_free = not s_valid or (s_emit and only one mask bit left)`

### T: traversal

```mermaid
flowchart TD
    T0["<b>T_IDLE</b> (t_active = 0)<br/>in_ready = not s_valid"] --> TL{"in_valid and not s_valid?"}
    TL -- no --> T0
    TL -- yes --> LD(["load: px0, px1, py1, t_x ← px0, t_y ← py0<br/>e_cur, e_row ← e0, a_cur, a_row ← a0<br/>exk[e][k] ← k·ex, exs ← SPAN·ex, ey<br/>axk[i][k] ← k·ax, axs ← SPAN·ax, ay, amin, amax<br/>t_active ← 1"])
    LD --> T1
    T1["<b>T_ACTIVE</b> (t_active = 1)<br/>compute mask for the span at (t_x, t_y)"] --> TS{"mask = 0 or s_free?"}
    TS -- no --> T1
    TS -- yes --> TM{"mask ≠ 0?"}
    TM -- yes --> HO(["hand-off to S:<br/>s_mask ← mask, s_x ← t_x<br/>s_y ← t_y, s_a ← a_cur"])
    TM -- no --> LS
    HO --> LS{"last span of row?<br/>t_x + SPAN above px1"}
    LS -- no --> NX(["t_x ← t_x + SPAN<br/>e_cur ← e_cur + exs<br/>a_cur ← a_cur + axs"])
    NX --> T1
    LS -- yes --> LR{"last row?<br/>t_y = py1"}
    LR -- yes --> DONE(["t_active ← 0"])
    DONE --> T0
    LR -- no --> NR(["t_x ← px0, t_y ← t_y + 1<br/>e_row ← e_row + ey, e_cur ← e_row + ey<br/>a_row ← a_row + ay, a_cur ← a_row + ay"])
    NR --> T1
```

### S: serializer and output register

```mermaid
flowchart TD
    E0["<b>S_EMPTY</b> (s_valid = 0)"] --> EH{"hand-off from T?"}
    EH -- no --> E0
    EH -- yes --> E1["<b>S_HOLD</b> (s_valid = 1)<br/>k = lowest set bit of s_mask"]
    E1 --> OF{"o_free?"}
    OF -- no --> E1
    OF -- yes --> EM(["frag_x ← s_x + k, frag_y ← s_y<br/>frag_z, r, g, b ← round and clamp(s_a[i] + axk[i][k])<br/>frag_valid ← 1<br/>s_mask ← s_mask without bit k"])
    EM --> LB{"was that the last bit?"}
    LB -- no --> E1
    LB -- yes --> NH{"hand-off from T this cycle?"}
    NH -- yes --> E1
    NH -- no --> E0
```

The output register clears (`frag_valid ← 0`) whenever `frag_ready = 1` and S
does not emit in that cycle.

---

## 9. `rop`: depth test, buffers and clear

### Clear engine

```mermaid
flowchart TD
    C0["<b>C_IDLE</b> (clear_busy = 0)<br/>in_ready = 1<br/>w_en[0] ← pass (from the fragment pipeline)"] --> CS{"start_clear?"}
    CS -- no --> C0
    CS -- yes --> CI(["clear_busy ← 1, clr_addr ← 0"])
    CI --> C1["<b>C_CLEAR</b> (clear_busy = 1)<br/>in_ready = 0<br/>w_en[0] ← 1, w_addr[0] ← clr_addr<br/>w_z[0] ← 0xFFFF, w_c ← clear_color<br/>clr_addr ← clr_addr + 1"]
    C1 --> CL{"clr_addr = NPIX − 1?"}
    CL -- no --> C1
    CL -- yes --> CD(["clear_busy ← 0"])
    CD --> C0
```

### Fragment pipeline, with the R3 forwarding decision

Every cycle, the write history shifts:
`w_*[2] ← w_*[1]`, `w_*[1] ← w_*[0]`. Entry 0 drives both RAMs' write ports.

```mermaid
flowchart TD
    R0["<b>R0</b> (accept when in_valid and in_ready)<br/>a1 ← y × W + x, z1 ← z<br/>c1 ← RGB565(r, g, b), v1 ← 1"] --> R1["<b>R1</b><br/>Z RAM read address ← a1<br/>(ext_addr when v1 = 0)<br/>a2, z2, c2, v2 ← stage 1"]
    R1 --> R2["<b>R2</b><br/>RAM access<br/>a3, z3, c3, v3 ← stage 2"]
    R2 --> H0{"<b>R3</b>: w_en[0] and<br/>w_addr[0] = a3?"}
    H0 -- yes --> Z0(["z_old ← w_z[0] (written 1 cycle ago)"])
    H0 -- no --> H1{"w_en[1] and w_addr[1] = a3?"}
    H1 -- yes --> Z1(["z_old ← w_z[1]"])
    H1 -- no --> H2{"w_en[2] and w_addr[2] = a3?"}
    H2 -- yes --> Z2(["z_old ← w_z[2]"])
    H2 -- no --> Z3(["z_old ← z_rdata (RAM)"])
    Z0 --> PS
    Z1 --> PS
    Z2 --> PS
    Z3 --> PS{"v3 and z3 below z_old?"}
    PS -- yes --> WP(["<b>W</b>: w_en[0] ← 1, w_addr[0] ← a3<br/>w_z[0] ← z3, w_c ← c3<br/>frag_pass ← 1"])
    PS -- no --> WN(["<b>W</b>: w_en[0] ← 0"])
```

The newest matching write wins, so the chart checks entries 0, 1, 2 in that
order. `idle = not (v1 or v2 or v3 or w_en[0] or clear_busy)`, so
`gpu_ctrl` only reports DONE after the final RAM write has committed.
