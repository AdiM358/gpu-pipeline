`default_nettype none

// Simple dual-port RAM (one write port, one read port), 2-cycle read latency.
//
// Written in the template Vivado maps to block RAM: synchronous write,
// registered read, plus an output register. For a deep memory built from
// many BRAM36 primitives, the second register sits after the cascade/output
// multiplexer, so the read path is not the critical path. No reset on the
// storage or the read registers (BRAM output registers are not reset here).
module sdp_ram #(
    parameter int WIDTH = 16,
    parameter int DEPTH = 320 * 240,
    parameter int AW    = $clog2(DEPTH)
)(
    input  wire              clk,

    input  wire              we,
    input  wire [AW-1:0]     waddr,
    input  wire [WIDTH-1:0]  wdata,

    input  wire [AW-1:0]     raddr,
    output logic [WIDTH-1:0] rdata      // mem[raddr] from two cycles earlier
);

    (* ram_style = "block" *) logic [WIDTH-1:0] mem [DEPTH];
    logic [WIDTH-1:0] rd_q;

    always_ff @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        rd_q  <= mem[raddr];
        rdata <= rd_q;
    end

endmodule

`default_nettype wire
