// Environment: one active agent, the scoreboard and the coverage collector.
//
//   monitor.frag_ap  --> scoreboard.frag_imp  --(decisions)--> coverage.dec_imp
//   monitor.pass_ap  --> scoreboard.pass_imp
//   monitor.clear_ap --> scoreboard.clear_imp, coverage.clear_imp

class rop_env extends uvm_env;
    `uvm_component_utils(rop_env)

    rop_agent      agent;
    rop_scoreboard sb;
    rop_coverage   cov;

    function new(string name, uvm_component parent);
        super.new(name, parent);
    endfunction

    function void build_phase(uvm_phase phase);
        agent = rop_agent::type_id::create("agent", this);
        sb    = rop_scoreboard::type_id::create("sb", this);
        cov   = rop_coverage::type_id::create("cov", this);
    endfunction

    function void connect_phase(uvm_phase phase);
        agent.mon.frag_ap.connect(sb.frag_imp);
        agent.mon.pass_ap.connect(sb.pass_imp);
        agent.mon.clear_ap.connect(sb.clear_imp);
        agent.mon.clear_ap.connect(cov.clear_imp);
        sb.dec_ap.connect(cov.dec_imp);
    endfunction
endclass
