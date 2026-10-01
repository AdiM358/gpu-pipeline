// Package holding the whole UVM environment for rtl/rop.sv.
`timescale 1ns/1ps
`include "uvm_macros.svh"

package rop_pkg;
    import uvm_pkg::*;

    // Must match the DUT parameters in tb_top.sv.
    localparam int SCREEN_W = 320;
    localparam int SCREEN_H = 240;
    localparam int NPIX     = SCREEN_W * SCREEN_H;

    // Fixed ROP timing, from rtl/rop.sv (checked by the scoreboard/test):
    //   a fragment accepted on cycle t has its frag_pass pulse sampled on t+4
    //   (R1, R2, R3 decision, then the registered frag_pass output);
    //   an ext_addr driven in one cycle returns data 2 loop iterations later.
    localparam int PASS_LATENCY = 4;
    localparam int READ_LATENCY = 2;

    // Analysis imp classes for components with more than one input.
    `uvm_analysis_imp_decl(_frag)
    `uvm_analysis_imp_decl(_pass)
    `uvm_analysis_imp_decl(_clear)
    `uvm_analysis_imp_decl(_dec)

    `include "rop_item.sv"
    `include "rop_sequences.sv"
    `include "rop_agent.sv"
    `include "rop_scoreboard.sv"
    `include "rop_coverage.sv"
    `include "rop_env.sv"
    `include "rop_tests.sv"
endpackage
