// Top level: clock, reset, the DUT and the interface; starts UVM.
// Select the test with +UVM_TESTNAME=<test> (default: rop_full_test).
`timescale 1ns/1ps
`include "uvm_macros.svh"

module tb_top;
    import uvm_pkg::*;
    import rop_pkg::*;

    logic clk = 1'b0;
    always #5 clk = ~clk;          // 100 MHz, the post-route Fmax

    rop_if vif (clk);

    rop #(.SCREEN_W(SCREEN_W), .SCREEN_H(SCREEN_H)) dut (
        .clk         (clk),
        .rst_n       (vif.rst_n),
        .in_x        (vif.in_x),
        .in_y        (vif.in_y),
        .in_z        (vif.in_z),
        .in_r        (vif.in_r),
        .in_g        (vif.in_g),
        .in_b        (vif.in_b),
        .in_valid    (vif.in_valid),
        .in_ready    (vif.in_ready),
        .start_clear (vif.start_clear),
        .clear_color (vif.clear_color),
        .clear_busy  (vif.clear_busy),
        .ext_addr    (vif.ext_addr),
        .ext_depth   (vif.ext_depth),
        .ext_data    (vif.ext_data),
        .frag_pass   (vif.frag_pass),
        .idle        (vif.idle)
    );

    initial begin
        vif.rst_n = 1'b0;
        repeat (5) @(posedge clk);
        vif.rst_n = 1'b1;
    end

    initial begin
        uvm_config_db #(virtual rop_if)::set(null, "*", "vif", vif);
        run_test("rop_full_test");
    end
endmodule
