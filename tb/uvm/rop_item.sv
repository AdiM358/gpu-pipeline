// Transactions.
//
// rop_item     one operation for the driver (a fragment or a clear), and also
//              what the monitor reports after observing one on the bus.
// rop_decision what the scoreboard predicted for one accepted fragment; it
//              is sent to the coverage collector.

typedef enum {OP_FRAG, OP_CLEAR} rop_op_e;
typedef enum {PASS, FAIL_GREATER, FAIL_EQUAL} rop_outcome_e;

class rop_item extends uvm_sequence_item;
    rand rop_op_e     op;
    rand bit [9:0]    x, y;
    rand bit [15:0]   z;
    rand bit [7:0]    r, g, b;
    rand bit [15:0]   clear_color;
    rand int unsigned gap;          // idle cycles (in_valid = 0) before this fragment

    // Filled in by the monitor, not randomized.
    longint unsigned  cycle;        // fragment: accept cycle; clear: cycle clear_busy fell
    int unsigned      busy_cycles;  // clear: cycles clear_busy was high

    // Precondition P2: the rasterizer only produces on-screen pixels.
    constraint c_on_screen   { x < SCREEN_W; y < SCREEN_H; }
    constraint c_gap_default { soft gap == 0; gap <= 16; }
    constraint c_op_default  { soft op == OP_FRAG; }

    `uvm_object_utils(rop_item)

    function new(string name = "rop_item");
        super.new(name);
    endfunction

    function string convert2string();
        if (op == OP_CLEAR)
            return $sformatf("CLEAR colour=%04h", clear_color);
        return $sformatf("FRAG (%0d,%0d) z=%04h rgb=%02h%02h%02h gap=%0d cycle=%0d",
                         x, y, z, r, g, b, gap, cycle);
    endfunction
endclass

class rop_decision extends uvm_object;
    bit [9:0]        x, y;
    bit [15:0]       z;
    rop_outcome_e    outcome;
    bit              pixel_drawn;   // pixel written by a fragment since the last clear
    int unsigned     distance;      // cycles since the last write to this pixel; 0 = none
    int unsigned     n_matches;     // writes to this pixel in the previous 3 cycles
    bit              gap;           // idle cycles between that last write and this fragment
    longint unsigned since_clear;   // cycles since the most recent clear finished

    `uvm_object_utils(rop_decision)

    function new(string name = "rop_decision");
        super.new(name);
    endfunction
endclass
