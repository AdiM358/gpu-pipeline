// Scoreboard: an in-order reference model of the ROP plus three checks.
//
//   1. frag_pass: for every accepted fragment the model decides pass/fail
//      immediately (no pipeline, so it can never read stale depth). Each
//      predicted write must produce exactly one frag_pass pulse exactly
//      PASS_LATENCY cycles after the fragment was accepted; missing, extra or
//      mistimed pulses are errors. This is also the forwarding check (F2).
//   2. clear: every clear must keep clear_busy high for exactly NPIX cycles.
//   3. readback: at end of test the test reads every pixel's colour and
//      depth through ext_addr and calls check_readback() for each.
//
// For every fragment it also sends a rop_decision (outcome, distance to the
// previous write to that pixel, ...) to the coverage collector.

class rop_scoreboard extends uvm_scoreboard;
    `uvm_component_utils(rop_scoreboard)

    uvm_analysis_imp_frag  #(rop_item, rop_scoreboard)         frag_imp;
    uvm_analysis_imp_pass  #(longint unsigned, rop_scoreboard) pass_imp;
    uvm_analysis_imp_clear #(rop_item, rop_scoreboard)         clear_imp;
    uvm_analysis_port      #(rop_decision)                     dec_ap;

    // Reference model state
    bit [15:0]       ref_z     [NPIX];
    bit [15:0]       ref_c     [NPIX];
    bit              drawn     [NPIX];   // written by a fragment since the last clear
    longint unsigned last_wr   [NPIX];   // accept cycle of the last write (0 = none)
    longint unsigned last_wr_n [NPIX];   // accepted-fragment count at that write
    bit              cleared_once;
    longint unsigned last_clear_end;

    // Writes accepted in the last few cycles, for the multi-match statistic.
    longint unsigned recent_cycle[$];
    int unsigned     recent_addr[$];

    longint unsigned exp_pass[$];        // cycles at which a frag_pass pulse is due

    // Recent decisions by accept cycle, so a bad pulse can name its fragment.
    rop_decision     by_cycle[longint unsigned];

    // Statistics
    int unsigned n_accepted, n_writes, n_pass_seen, n_clears;
    int unsigned n_outcome[3];
    int unsigned n_rb_checked, n_rb_bad;

    function new(string name, uvm_component parent);
        super.new(name, parent);
        frag_imp  = new("frag_imp", this);
        pass_imp  = new("pass_imp", this);
        clear_imp = new("clear_imp", this);
        dec_ap    = new("dec_ap", this);
    endfunction

    static function bit [15:0] rgb565(bit [7:0] r, bit [7:0] g, bit [7:0] b);
        return {r[7:3], g[7:2], b[7:3]};
    endfunction

    function void write_frag(rop_item t);
        int unsigned  addr = t.y * SCREEN_W + t.x;
        bit [15:0]    z_old = ref_z[addr];
        rop_decision  d = rop_decision::type_id::create("d");

        if (!cleared_once)
            `uvm_error("SB", "fragment accepted before the first clear: model state unknown")

        d.x = t.x;  d.y = t.y;  d.z = t.z;
        d.outcome     = (t.z < z_old) ? PASS : (t.z == z_old) ? FAIL_EQUAL : FAIL_GREATER;
        d.pixel_drawn = drawn[addr];
        d.since_clear = t.cycle - last_clear_end;

        // Distance to the previous write to this pixel, and whether any idle
        // cycles fell in between (cycle gap larger than fragment gap).
        if (last_wr[addr] != 0) begin
            d.distance = t.cycle - last_wr[addr];
            d.gap  = (t.cycle - last_wr[addr]) != (n_accepted - last_wr_n[addr]);
        end

        // How many writes to this pixel are still inside the 3-cycle window.
        while (recent_cycle.size() > 0 && recent_cycle[0] + 3 < t.cycle) begin
            void'(recent_cycle.pop_front());
            void'(recent_addr.pop_front());
        end
        foreach (recent_addr[i]) if (recent_addr[i] == addr) d.n_matches++;

        if (d.outcome == PASS) begin
            ref_z[addr]     = t.z;
            ref_c[addr]     = rgb565(t.r, t.g, t.b);
            drawn[addr]     = 1;
            last_wr[addr]   = t.cycle;
            last_wr_n[addr] = n_accepted;
            recent_cycle.push_back(t.cycle);
            recent_addr.push_back(addr);
            exp_pass.push_back(t.cycle + PASS_LATENCY);
            n_writes++;
        end
        n_outcome[d.outcome]++;
        n_accepted++;
        by_cycle[t.cycle] = d;
        if (by_cycle.exists(t.cycle - 16)) by_cycle.delete(t.cycle - 16);
        dec_ap.write(d);
    endfunction

    // Describes the fragment whose decision lands on a given frag_pass cycle.
    function string who(longint unsigned pass_cycle);
        longint unsigned acc = pass_cycle - PASS_LATENCY;
        rop_decision d;
        if (!by_cycle.exists(acc)) return "no fragment was accepted at that time";
        d = by_cycle[acc];
        return $sformatf("fragment (%0d,%0d) z=%04h accepted at cycle %0d, model says %s, distance to last write %0d, %0d in-flight matches",
                         d.x, d.y, d.z, acc, d.outcome.name(), d.distance, d.n_matches);
    endfunction

    function void write_pass(longint unsigned cycle);
        while (exp_pass.size() > 0 && exp_pass[0] < cycle) begin
            `uvm_error("FRAG_PASS", $sformatf("missing frag_pass pulse at cycle %0d: %s", exp_pass[0], who(exp_pass[0])))
            void'(exp_pass.pop_front());
        end
        if (exp_pass.size() == 0 || exp_pass[0] != cycle) begin
            `uvm_error("FRAG_PASS", $sformatf("unexpected frag_pass pulse at cycle %0d: %s", cycle, who(cycle)))
            return;
        end
        void'(exp_pass.pop_front());
        n_pass_seen++;
    endfunction

    function void write_clear(rop_item c);
        if (c.busy_cycles != NPIX)
            `uvm_error("CLEAR", $sformatf("clear_busy was high for %0d cycles, expected %0d",
                                          c.busy_cycles, NPIX))
        if (exp_pass.size() != 0)
            `uvm_error("CLEAR", "clear finished with frag_pass pulses still outstanding")
        foreach (ref_z[i]) begin
            ref_z[i]   = 16'hFFFF;
            ref_c[i]   = c.clear_color;
            drawn[i]   = 0;
            last_wr[i] = 0;
        end
        recent_cycle.delete();
        recent_addr.delete();
        cleared_once   = 1;
        last_clear_end = c.cycle;
        n_clears++;
    endfunction

    function void check_readback(int unsigned addr, bit depth, bit [15:0] got);
        bit [15:0] exp = depth ? ref_z[addr] : ref_c[addr];
        n_rb_checked++;
        if (got !== exp) begin
            n_rb_bad++;
            if (n_rb_bad <= 10)
                `uvm_error("READBACK", $sformatf("pixel (%0d,%0d) %s: read %04h, expected %04h",
                           addr % SCREEN_W, addr / SCREEN_W, depth ? "depth" : "colour", got, exp))
        end
    endfunction

    function void check_phase(uvm_phase phase);
        foreach (exp_pass[i])
            `uvm_error("FRAG_PASS", $sformatf("missing frag_pass pulse due at cycle %0d", exp_pass[i]))
        if (n_pass_seen != n_writes)
            `uvm_error("FRAG_PASS", $sformatf("%0d frag_pass pulses for %0d predicted writes",
                                              n_pass_seen, n_writes))
    endfunction

    function void report_phase(uvm_phase phase);
        `uvm_info("SB", $sformatf({"accepted %0d fragments (pass %0d, fail_greater %0d, ",
                  "fail_equal %0d); frag_pass pulses matched %0d/%0d; clears %0d; ",
                  "readback %0d values checked, %0d mismatches"},
                  n_accepted, n_outcome[PASS], n_outcome[FAIL_GREATER], n_outcome[FAIL_EQUAL],
                  n_pass_seen, n_writes, n_clears, n_rb_checked, n_rb_bad), UVM_NONE)
    endfunction
endclass
