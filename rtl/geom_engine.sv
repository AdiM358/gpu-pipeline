`default_nettype none

// MVP transform, rate-matched to vertex fetch.
//
// The 32-bit fetch bus delivers at most one 16-byte vertex every 4 cycles, so
// instead of 16 multipliers computing a whole vertex per cycle, 4 multipliers
// compute one output row (one dot product) per cycle: 4 cycles per vertex,
// the same throughput with a quarter of the DSPs.
//
//   clip[r] = bits [47:16] of  sum_c M[r][c] * v[c],   v = (x, y, z, 1.0)
//
// Pipeline (all stages advance together while `en`):
//   I  operand select   a[c] = M[row][c], b[c] = v[c]
//   P1 multiply         p[c] = a[c] * b[c]           (48 LSBs kept)
//   P2 product register (lets synthesis use the DSP's M and P registers)
//   S  pairwise sums
//   F  final sum; rows 0..2 wait in acc[], row 3 completes the output vertex
module geom_engine (
    input  wire                clk,
    input  wire                rst_n,

    input  wire signed [31:0]  mvp [4][4],   // Q16.16, row-major

    input  wire signed [31:0]  in_x, in_y, in_z,
    input  wire        [23:0]  in_color,
    input  wire                in_valid,
    output logic               in_ready,

    output logic signed [31:0] out_x, out_y, out_z, out_w,
    output logic       [23:0]  out_color,
    output logic               out_valid,
    input  wire                out_ready,

    output logic               idle
);

    localparam logic signed [31:0] ONE = 32'sh0001_0000;

    wire en = !out_valid || out_ready;

    // ---------------------------------------------------------------- issue
    logic               busy;       // a vertex is being issued row by row
    logic [1:0]         row;
    logic signed [31:0] v [4];
    logic [23:0]        color_i;

    assign in_ready = en && (!busy || row == 2'd3);
    wire   accept   = in_valid && in_ready;

    // ---------------------------------------------------------------- stage regs
    logic               vld_i, vld_p1, vld_p2, vld_s;
    logic [1:0]         row_i, row_p1, row_p2, row_s;
    logic [23:0]        col_i, col_p1, col_p2, col_s;   // valid with row 3
    logic signed [31:0] a_i [4], b_i [4];
    logic signed [47:0] p1 [4], p2 [4];
    logic signed [47:0] s01, s23;
    logic signed [31:0] acc [3];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            busy      <= 1'b0;
            row       <= '0;
            vld_i     <= 1'b0;
            vld_p1    <= 1'b0;
            vld_p2    <= 1'b0;
            vld_s     <= 1'b0;
            out_valid <= 1'b0;
        end else if (en) begin
            // Issue: a new vertex starts at row 0; otherwise advance the row.
            if (accept) begin
                busy <= 1'b1;
                row  <= 2'd0;
            end else if (busy) begin
                row <= row + 1'b1;
                if (row == 2'd3) busy <= 1'b0;
            end
            vld_i  <= busy;
            vld_p1 <= vld_i;
            vld_p2 <= vld_p1;
            vld_s  <= vld_p2;
            if (out_valid && out_ready) out_valid <= 1'b0;
            if (vld_s && row_s == 2'd3) out_valid <= 1'b1;
        end
    end

    // Data path: no reset needed (qualified by the valid bits above).
    always_ff @(posedge clk) begin
        if (en) begin
            if (accept) begin
                v[0]    <= in_x;
                v[1]    <= in_y;
                v[2]    <= in_z;
                v[3]    <= ONE;
                color_i <= in_color;
            end
            // I
            for (int c = 0; c < 4; c++) begin
                a_i[c] <= mvp[row][c];
                b_i[c] <= v[c];
            end
            row_i <= row;
            col_i <= color_i;
            // P1, P2
            for (int c = 0; c < 4; c++) begin
                // Only bits [47:16] of the final sum are kept, so the upper
                // product bits can never affect the result.
                /* verilator lint_off UNUSEDSIGNAL */
                automatic logic signed [63:0] prod = a_i[c] * b_i[c];  // 32x32 -> 64
                /* verilator lint_on UNUSEDSIGNAL */
                p1[c] <= prod[47:0];
                p2[c] <= p1[c];
            end
            row_p1 <= row_i;
            col_p1 <= col_i;
            row_p2 <= row_p1;
            col_p2 <= col_p1;
            // S
            s01   <= p2[0] + p2[1];
            s23   <= p2[2] + p2[3];
            row_s <= row_p2;
            col_s <= col_p2;
            // F
            if (vld_s) begin
                /* verilator lint_off UNUSEDSIGNAL */
                automatic logic signed [47:0] sum = s01 + s23;  // Q32.32 -> keep Q16.16
                /* verilator lint_on UNUSEDSIGNAL */
                if (row_s != 2'd3) begin
                    acc[row_s] <= sum[47:16];
                end else begin
                    out_x     <= acc[0];
                    out_y     <= acc[1];
                    out_z     <= acc[2];
                    out_w     <= sum[47:16];
                    out_color <= col_s;
                end
            end
        end
    end

    assign idle = !busy && !vld_i && !vld_p1 && !vld_p2 && !vld_s && !out_valid;

endmodule

`default_nettype wire
