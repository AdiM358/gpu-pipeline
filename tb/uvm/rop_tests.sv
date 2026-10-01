// Tests. rop_base_test does the common work:
//   reset -> initial clear -> run_body() -> drain -> framebuffer readback
//   -> PASS/FAIL verdict from the UVM error count.
// Each derived test only overrides run_body().

class rop_base_test extends uvm_test;
    `uvm_component_utils(rop_base_test)

    rop_env        env;
    virtual rop_if vif;

    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction

    function void build_phase(uvm_phase phase);
        env = rop_env::type_id::create("env", this);
        if (!uvm_config_db #(virtual rop_if)::get(this, "", "vif", vif))
            `uvm_fatal("NOVIF", "rop_if not set in config_db")
    endfunction

    // Derived tests put their stimulus here.
    virtual task run_body();
    endtask

    task run_phase(uvm_phase phase);
        rop_clear_seq clr = rop_clear_seq::type_id::create("init_clear");
        phase.raise_objection(this);
        wait (vif.rst_n === 1'b1);
        repeat (2) @(vif.drv_cb);
        // The framebuffer RAMs power up with unknown contents, so every test
        // starts by clearing them; the model knows nothing until then.
        if (!clr.randomize()) `uvm_fatal("RAND", "clear colour randomization failed")
        clr.start(env.agent.sqr);
        run_body();
        drain();
        readback();
        phase.drop_objection(this);
    endtask

    // Wait for the last fragment to finish (frag_pass pulses arrive late).
    task drain();
        repeat (8) @(vif.drv_cb);
        while (!vif.drv_cb.idle) @(vif.drv_cb);
        repeat (8) @(vif.drv_cb);
    endtask

    // F7: read every pixel's colour, then its depth, through the external
    // port while the ROP is idle (precondition P3), one address per cycle.
    // An address driven in loop iteration i is sampled READ_LATENCY
    // iterations later.
    task readback();
        for (int depth = 0; depth < 2; depth++) begin
            vif.drv_cb.ext_depth <= depth[0];
            for (int i = 0; i < NPIX + READ_LATENCY; i++) begin
                if (i < NPIX) vif.drv_cb.ext_addr <= 17'(i);
                @(vif.drv_cb);
                if (i >= READ_LATENCY)
                    env.sb.check_readback(i - READ_LATENCY, depth[0], vif.drv_cb.ext_data);
            end
        end
    endtask

    function void check_phase(uvm_phase phase);
        // F4: every fragment the driver sent was accepted exactly once.
        if (env.agent.drv.n_frags_driven != env.sb.n_accepted)
            `uvm_error("F4", $sformatf("driver sent %0d fragments, monitor saw %0d accepted",
                                       env.agent.drv.n_frags_driven, env.sb.n_accepted))
    endfunction

    function void report_phase(uvm_phase phase);
        uvm_report_server rs = uvm_report_server::get_server();
        int errors = rs.get_severity_count(UVM_ERROR) + rs.get_severity_count(UVM_FATAL);
        `uvm_info("RESULT", $sformatf("%s: %s with %0d errors", get_type_name(),
                  errors == 0 ? "TEST PASSED" : "TEST FAILED", errors), UVM_NONE)
    endfunction
endclass

class rop_random_test extends rop_base_test;
    `uvm_component_utils(rop_random_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_random_seq s = rop_random_seq::type_id::create("s");
        s.n_frags = 3000;
        s.max_gap = 3;
        s.start(env.agent.sqr);
    endtask
endclass

class rop_hazard_test extends rop_base_test;
    `uvm_component_utils(rop_hazard_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_hazard_seq s = rop_hazard_seq::type_id::create("s");
        s.n_frags = 5000;
        s.start(env.agent.sqr);
    endtask
endclass

class rop_gap_test extends rop_base_test;
    `uvm_component_utils(rop_gap_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_hazard_seq s = rop_hazard_seq::type_id::create("s");
        s.n_frags = 3000;
        s.pool    = 2;
        s.max_gap = 8;
        s.start(env.agent.sqr);
    endtask
endclass

class rop_clear_draw_test extends rop_base_test;
    `uvm_component_utils(rop_clear_draw_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_clear_draw_seq s = rop_clear_draw_seq::type_id::create("s");
        s.start(env.agent.sqr);
    endtask
endclass

class rop_fwd_directed_test extends rop_base_test;
    `uvm_component_utils(rop_fwd_directed_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_fwd_directed_seq s = rop_fwd_directed_seq::type_id::create("s");
        s.start(env.agent.sqr);
    endtask
endclass

// Everything in one simulation, so the coverage numbers describe the full
// suite (the free Vivado licence cannot merge coverage across runs).
class rop_full_test extends rop_base_test;
    `uvm_component_utils(rop_full_test)
    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction
    task run_body();
        rop_fwd_directed_seq dir = rop_fwd_directed_seq::type_id::create("dir");
        rop_random_seq       rnd = rop_random_seq::type_id::create("rnd");
        rop_hazard_seq       hz  = rop_hazard_seq::type_id::create("hz");
        rop_hazard_seq       gp  = rop_hazard_seq::type_id::create("gp");
        rop_edge_seq         edg = rop_edge_seq::type_id::create("edg");
        rop_clear_draw_seq   cd  = rop_clear_draw_seq::type_id::create("cd");
        dir.start(env.agent.sqr);
        rnd.n_frags = 3000;  rnd.max_gap = 3;
        rnd.start(env.agent.sqr);
        hz.n_frags = 5000;
        hz.start(env.agent.sqr);
        gp.n_frags = 3000;   gp.pool = 2;  gp.max_gap = 8;
        gp.start(env.agent.sqr);
        edg.start(env.agent.sqr);
        cd.start(env.agent.sqr);
    endtask
endclass
