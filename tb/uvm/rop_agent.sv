// Driver, monitor and agent for the ROP's fragment, clear and status pins.
// (The external read port is driven directly by the test's end-of-test
// readback, see rop_tests.sv.)

typedef uvm_sequencer #(rop_item) rop_sequencer;

class rop_driver extends uvm_driver #(rop_item);
    `uvm_component_utils(rop_driver)

    virtual rop_if vif;
    int unsigned   n_frags_driven;

    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction

    function void build_phase(uvm_phase phase);
        if (!uvm_config_db #(virtual rop_if)::get(this, "", "vif", vif))
            `uvm_fatal("NOVIF", "rop_if not set in config_db")
    endfunction

    task run_phase(uvm_phase phase);
        vif.drv_cb.in_valid    <= 1'b0;
        vif.drv_cb.start_clear <= 1'b0;
        vif.drv_cb.ext_addr    <= '0;
        vif.drv_cb.ext_depth   <= 1'b0;
        wait (vif.rst_n === 1'b1);
        @(vif.drv_cb);
        forever begin
            seq_item_port.get_next_item(req);
            if (req.op == OP_CLEAR) drive_clear(req);
            else                    drive_frag(req);
            seq_item_port.item_done();
        end
    endtask

    // Valid/ready: hold valid and data until the DUT samples ready = 1.
    task drive_frag(rop_item t);
        if (t.gap > 0) begin
            vif.drv_cb.in_valid <= 1'b0;
            repeat (t.gap) @(vif.drv_cb);
        end
        vif.drv_cb.in_x     <= t.x;
        vif.drv_cb.in_y     <= t.y;
        vif.drv_cb.in_z     <= t.z;
        vif.drv_cb.in_r     <= t.r;
        vif.drv_cb.in_g     <= t.g;
        vif.drv_cb.in_b     <= t.b;
        vif.drv_cb.in_valid <= 1'b1;
        @(vif.drv_cb);
        while (!vif.drv_cb.in_ready) @(vif.drv_cb);   // accepted on this edge
        // Drop valid; if the next item is a back-to-back fragment it raises
        // valid again in this same time step and the later write wins.
        vif.drv_cb.in_valid <= 1'b0;
        n_frags_driven++;
    endtask

    // Precondition P1: a clear may only start while the ROP is idle, so
    // stop sending fragments and wait for idle first.
    task drive_clear(rop_item t);
        vif.drv_cb.in_valid <= 1'b0;
        @(vif.drv_cb);
        while (!vif.drv_cb.idle) @(vif.drv_cb);
        vif.drv_cb.clear_color <= t.clear_color;
        vif.drv_cb.start_clear <= 1'b1;
        @(vif.drv_cb);
        vif.drv_cb.start_clear <= 1'b0;
        @(vif.drv_cb);
        while (vif.drv_cb.clear_busy) @(vif.drv_cb);
    endtask
endclass

class rop_monitor extends uvm_monitor;
    `uvm_component_utils(rop_monitor)

    virtual rop_if vif;
    uvm_analysis_port #(rop_item)         frag_ap;   // every accepted fragment
    uvm_analysis_port #(longint unsigned) pass_ap;   // cycle of every frag_pass pulse
    uvm_analysis_port #(rop_item)         clear_ap;  // every completed clear

    longint unsigned cycle;   // clock cycles since reset was released

    function new(string name, uvm_component parent);
        super.new(name, parent);
        frag_ap  = new("frag_ap", this);
        pass_ap  = new("pass_ap", this);
        clear_ap = new("clear_ap", this);
    endfunction

    function void build_phase(uvm_phase phase);
        if (!uvm_config_db #(virtual rop_if)::get(this, "", "vif", vif))
            `uvm_fatal("NOVIF", "rop_if not set in config_db")
    endfunction

    task run_phase(uvm_phase phase);
        bit          clear_pending = 0;
        bit [15:0]   clear_color;
        int unsigned busy_cycles   = 0;
        forever begin
            @(vif.mon_cb);
            if (vif.mon_cb.rst_n !== 1'b1) continue;
            cycle++;

            // F4: nothing may be accepted while a clear is running.
            if (vif.mon_cb.clear_busy && vif.mon_cb.in_ready)
                `uvm_error("F4", "in_ready is high while clear_busy")
            // P1 sanity: our own stimulus must never break the precondition.
            if (vif.mon_cb.start_clear && !vif.mon_cb.idle)
                `uvm_error("P1", "start_clear asserted while the ROP is not idle")

            if (vif.mon_cb.in_valid && vif.mon_cb.in_ready) begin
                rop_item t = rop_item::type_id::create("observed");
                t.op    = OP_FRAG;
                t.x     = vif.mon_cb.in_x;
                t.y     = vif.mon_cb.in_y;
                t.z     = vif.mon_cb.in_z;
                t.r     = vif.mon_cb.in_r;
                t.g     = vif.mon_cb.in_g;
                t.b     = vif.mon_cb.in_b;
                t.cycle = cycle;
                frag_ap.write(t);
            end

            if (vif.mon_cb.frag_pass) pass_ap.write(cycle);

            // Clear: start_clear seen, then count clear_busy cycles until it falls.
            if (vif.mon_cb.start_clear && !clear_pending) begin
                clear_pending = 1;
                clear_color   = vif.mon_cb.clear_color;
                busy_cycles   = 0;
            end else if (clear_pending) begin
                if (vif.mon_cb.clear_busy) busy_cycles++;
                else if (busy_cycles > 0) begin
                    rop_item c = rop_item::type_id::create("observed_clear");
                    c.op          = OP_CLEAR;
                    c.clear_color = clear_color;
                    c.busy_cycles = busy_cycles;
                    c.cycle       = cycle;
                    clear_ap.write(c);
                    clear_pending = 0;
                end
            end
        end
    endtask
endclass

class rop_agent extends uvm_agent;
    `uvm_component_utils(rop_agent)

    rop_sequencer sqr;
    rop_driver    drv;
    rop_monitor   mon;

    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction

    function void build_phase(uvm_phase phase);
        sqr = rop_sequencer::type_id::create("sqr", this);
        drv = rop_driver::type_id::create("drv", this);
        mon = rop_monitor::type_id::create("mon", this);
    endfunction

    function void connect_phase(uvm_phase phase);
        drv.seq_item_port.connect(sqr.seq_item_export);
    endfunction
endclass
