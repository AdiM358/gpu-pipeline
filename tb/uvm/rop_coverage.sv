// Functional coverage. Each covergroup maps to a feature in
// docs/VERIFICATION_PLAN.md. Sampled from the scoreboard's per-fragment
// decisions and from the monitor's completed clears.

class rop_coverage extends uvm_component;
    `uvm_component_utils(rop_coverage)

    uvm_analysis_imp_dec   #(rop_decision, rop_coverage) dec_imp;
    uvm_analysis_imp_clear #(rop_item, rop_coverage)     clear_imp;

    typedef enum {D1, D2, D3, D4, D_FAR} dist_e;

    int unsigned n_decisions;   // fragments seen so far (for "clear after traffic")

    // F1: depth test
    covergroup cg_depth_test with function sample(rop_outcome_e o, bit [15:0] z, bit drawn);
        cp_outcome: coverpoint o {
            bins pass         = {PASS};
            bins fail_greater = {FAIL_GREATER};
            bins fail_equal   = {FAIL_EQUAL};
        }
        cp_z: coverpoint z {
            bins z_zero = {16'h0000};
            bins z_max  = {16'hFFFF};
            bins z_mid  = {[16'h0001:16'hFFFE]};
        }
        cp_pixel_state: coverpoint drawn {
            bins cleared = {0};
            bins drawn   = {1};
        }
        cx_outcome_z: cross cp_outcome, cp_z {
            // z = 0xFFFF can never be strictly less than a 16-bit depth.
            ignore_bins pass_at_max  = binsof(cp_outcome.pass) && binsof(cp_z.z_max);
            // z = 0 can never be greater than a stored depth.
            ignore_bins greater_at_0 = binsof(cp_outcome.fail_greater) && binsof(cp_z.z_zero);
        }
    endgroup

    // F2 / F3: read-after-write forwarding, distance in cycles
    covergroup cg_forwarding with function sample(dist_e d, int unsigned m, bit gap, rop_outcome_e o);
        cp_distance: coverpoint d {
            bins d1  = {D1};
            bins d2  = {D2};
            bins d3  = {D3};
            bins d4  = {D4};      // first distance served by the RAM, not forwarding
            bins far = {D_FAR};
        }
        cp_matches: coverpoint m {
            bins none     = {0};
            bins one      = {1};
            bins multiple = {[2:3]};   // two or more in-flight writes to the pixel
        }
        cp_gap: coverpoint gap iff (d != D_FAR) {
            bins back_to_back = {0};
            bins with_idle    = {1};
        }
        cp_outcome: coverpoint o {
            bins pass         = {PASS};
            bins fail_greater = {FAIL_GREATER};
            bins fail_equal   = {FAIL_EQUAL};
        }
        cx_distance_outcome: cross cp_distance, cp_outcome;
    endgroup

    // F6: screen edges and corners
    covergroup cg_edges with function sample(int unsigned x, int unsigned y);
        cp_x: coverpoint x {
            bins left  = {0};
            bins right = {SCREEN_W - 1};
            bins mid   = {[1:SCREEN_W - 2]};
        }
        cp_y: coverpoint y {
            bins top    = {0};
            bins bottom = {SCREEN_H - 1};
            bins mid    = {[1:SCREEN_H - 2]};
        }
        cx_corners: cross cp_x, cp_y;
    endgroup

    // F5: clears, and draw traffic right after a clear
    covergroup cg_clear with function sample(bit [15:0] color, bit after_traffic);
        cp_color: coverpoint color {
            bins black = {16'h0000};
            bins white = {16'hFFFF};
            bins other = {[16'h0001:16'hFFFE]};
        }
        cp_after_traffic: coverpoint after_traffic {
            bins first_clear   = {0};
            bins after_drawing = {1};
        }
    endgroup

    // Sampled as a 32-bit value clamped to SINCE_MAX: with a 64-bit sample and
    // an open-ended [17:$] bin, xsim rewrote the bin to [17:0].
    localparam int unsigned SINCE_MAX = 32'hFFFF_FFFF;
    covergroup cg_after_clear with function sample(int unsigned since);
        cp_since_clear: coverpoint since {
            bins immediate = {[1:4]};     // first fragments right after clear_busy falls
            bins soon      = {[5:16]};
            bins later     = {[17:SINCE_MAX]};
        }
    endgroup

    function new(string name, uvm_component parent);
        super.new(name, parent);
        dec_imp        = new("dec_imp", this);
        clear_imp      = new("clear_imp", this);
        cg_depth_test  = new();
        cg_forwarding  = new();
        cg_edges       = new();
        cg_clear       = new();
        cg_after_clear = new();
    endfunction

    function void write_dec(rop_decision d);
        dist_e dbin = (d.distance == 0 || d.distance > 4) ? D_FAR : dist_e'(d.distance - 1);
        cg_depth_test.sample(d.outcome, d.z, d.pixel_drawn);
        cg_forwarding.sample(dbin, d.n_matches, d.gap, d.outcome);
        cg_edges.sample(d.x, d.y);
        cg_after_clear.sample(d.since_clear > SINCE_MAX ? SINCE_MAX : int'(d.since_clear));
        n_decisions++;
    endfunction

    function void write_clear(rop_item c);
        cg_clear.sample(c.clear_color, n_decisions > 0);
    endfunction

    function void report_phase(uvm_phase phase);
        `uvm_info("COV", $sformatf("cg_depth_test  %6.2f%%  (outcome %0.2f, z %0.2f, pixel state %0.2f, outcome x z %0.2f)",
                  cg_depth_test.get_inst_coverage(), cg_depth_test.cp_outcome.get_inst_coverage(),
                  cg_depth_test.cp_z.get_inst_coverage(), cg_depth_test.cp_pixel_state.get_inst_coverage(),
                  cg_depth_test.cx_outcome_z.get_inst_coverage()), UVM_NONE)
        `uvm_info("COV", $sformatf("cg_forwarding  %6.2f%%  (distance %0.2f, matches %0.2f, gap %0.2f, distance x outcome %0.2f)",
                  cg_forwarding.get_inst_coverage(), cg_forwarding.cp_distance.get_inst_coverage(),
                  cg_forwarding.cp_matches.get_inst_coverage(), cg_forwarding.cp_gap.get_inst_coverage(),
                  cg_forwarding.cx_distance_outcome.get_inst_coverage()), UVM_NONE)
        `uvm_info("COV", $sformatf("cg_edges       %6.2f%%  (corners cross %0.2f)",
                  cg_edges.get_inst_coverage(), cg_edges.cx_corners.get_inst_coverage()), UVM_NONE)
        `uvm_info("COV", $sformatf("cg_clear       %6.2f%%", cg_clear.get_inst_coverage()), UVM_NONE)
        `uvm_info("COV", $sformatf("cg_after_clear %6.2f%%", cg_after_clear.get_inst_coverage()), UVM_NONE)
    endfunction
endclass
