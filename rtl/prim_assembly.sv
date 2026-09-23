`default_nettype none

// Triangle-list assembly: groups every 3 screen-space vertices into one
// triangle. The first two vertices wait in holding registers; the third is
// combined with them straight into the output register, so a new triangle can
// start the cycle after the previous one is handed off (no bubble).
//
// `flush` (pulsed at the start of each draw) discards a partial triangle left
// over from a draw whose vertex count was not a multiple of 3.
module prim_assembly (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               flush,

    input  wire signed [15:0] in_sx, in_sy,
    input  wire        [15:0] in_z,
    input  wire        [23:0] in_color,
    input  wire        [7:0]  in_flags,
    input  wire               in_valid,
    output logic              in_ready,

    output logic signed [15:0] out_sx [3],
    output logic signed [15:0] out_sy [3],
    output logic        [15:0] out_z [3],
    output logic        [23:0] out_color [3],
    output logic        [7:0]  out_flags [3],
    output logic               out_valid,
    input  wire                out_ready,

    output logic               idle
);

    logic [1:0] count;   // vertices held (0..2)
    logic signed [15:0] h_sx [2], h_sy [2];
    logic [15:0]        h_z [2];
    logic [23:0]        h_color [2];
    logic [7:0]         h_flags [2];

    wire third = (count == 2'd2);
    // Only the third vertex needs the output register to be free.
    assign in_ready = !third || !out_valid || out_ready;
    wire accept = in_valid && in_ready;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count     <= '0;
            out_valid <= 1'b0;
        end else begin
            if (out_valid && out_ready) out_valid <= 1'b0;
            if (flush) begin
                count <= '0;
            end else if (accept) begin
                count <= third ? 2'd0 : count + 1'b1;
                if (third) out_valid <= 1'b1;
            end
        end
    end

    always_ff @(posedge clk) begin
        if (accept && !flush) begin
            if (!third) begin
                h_sx[count[0]]    <= in_sx;
                h_sy[count[0]]    <= in_sy;
                h_z[count[0]]     <= in_z;
                h_color[count[0]] <= in_color;
                h_flags[count[0]] <= in_flags;
            end else begin
                out_sx    <= '{h_sx[0], h_sx[1], in_sx};
                out_sy    <= '{h_sy[0], h_sy[1], in_sy};
                out_z     <= '{h_z[0], h_z[1], in_z};
                out_color <= '{h_color[0], h_color[1], in_color};
                out_flags <= '{h_flags[0], h_flags[1], in_flags};
            end
        end
    end

    // A partial triangle does not count as work in flight: it can only be
    // completed by more vertices, and is discarded by the next flush.
    assign idle = !out_valid;

endmodule

`default_nettype wire
