// Interface between the UVM environment and rtl/rop.sv.
//
// Two clocking blocks give the testbench race-free timing:
//   drv_cb: the driver writes outputs 1 ns after the clock edge and reads
//           inputs as they were just before the edge (#1step).
//   mon_cb: the monitor only reads, also just before the edge, so it sees
//           exactly the values the DUT sampled on that edge.
`timescale 1ns/1ps

interface rop_if (input logic clk);
    logic        rst_n;

    // Fragment port
    logic [9:0]  in_x, in_y;
    logic [15:0] in_z;
    logic [7:0]  in_r, in_g, in_b;
    logic        in_valid;
    logic        in_ready;

    // Clear engine
    logic        start_clear;
    logic [15:0] clear_color;
    logic        clear_busy;

    // External read port (2-cycle latency)
    logic [16:0] ext_addr;
    logic        ext_depth;
    logic [15:0] ext_data;

    // Status
    logic        frag_pass;
    logic        idle;

    clocking drv_cb @(posedge clk);
        default input #1step output #1;
        output in_x, in_y, in_z, in_r, in_g, in_b, in_valid;
        output start_clear, clear_color, ext_addr, ext_depth;
        input  in_ready, clear_busy, idle, ext_data;
    endclocking

    clocking mon_cb @(posedge clk);
        default input #1step;
        input rst_n, in_x, in_y, in_z, in_r, in_g, in_b, in_valid, in_ready;
        input start_clear, clear_color, clear_busy, frag_pass, idle;
    endclocking
endinterface
