`default_nettype none

// Triangle setup (bit-exact with model::setup in model/gpu_model.cpp).
//
//   CLASS  trivial reject (all three outcodes share a plane), NEAR, guard band
//   AREA   doubled signed area from two 16x16 products
//   DECIDE cull (zero area, or back-facing when enabled), empty bounding box,
//          winding normalised to positive by swapping v1/v2
//   PREP   edge vectors, top-left flags, attribute deltas; start 1/area
//   RUN    38 operations through one shared multiplier:
//            6  edge functions at the first pixel centre
//           16  gradient numerators (4 attributes x 2 axes x 2 terms)
//            8  gradients = numerator * (1/area), normalised shift
//            8  attribute start values = a0 + Gx*dx + Gy*dy
//   OUT    hand the triangle to the rasterizer
//
// 1/area is computed by a 25-iteration restoring divider on the normalised
// area (A << clz(A)), so a single 25-bit reciprocal serves every triangle
// size. Setup takes ~55 cycles; it overlaps with rasterisation of the
// previous triangle because the result waits in an output register.
module tri_setup #(
    parameter int SCREEN_W = 320,
    parameter int SCREEN_H = 240
)(
    input  wire                clk,
    input  wire                rst_n,
    input  wire                cull_back,
    input  wire                front_cw,

    input  wire signed [15:0]  in_sx [3],
    input  wire signed [15:0]  in_sy [3],
    input  wire        [15:0]  in_z [3],
    input  wire        [23:0]  in_color [3],
    input  wire        [7:0]   in_flags [3],
    input  wire                in_valid,
    output logic               in_ready,

    output logic       [9:0]   out_px0, out_px1, out_py0, out_py1,
    output logic signed [33:0] out_e0 [3],
    output logic signed [20:0] out_ex [3],
    output logic signed [20:0] out_ey [3],
    output logic signed [37:0] out_a0 [4],
    output logic signed [37:0] out_ax [4],
    output logic signed [37:0] out_ay [4],
    output logic       [15:0]  out_amin [4],
    output logic       [15:0]  out_amax [4],
    output logic               out_valid,
    input  wire                out_ready,

    output logic               culled,    // pulse per culled triangle
    output logic               clipped,   // pulse per rejected triangle
    output logic               idle
);

    localparam logic [5:0] NOPS = 6'd38;

    typedef enum logic [2:0] {S_IDLE, S_CLASS, S_AREA, S_DECIDE, S_PREP, S_RUN, S_OUT} state_t;
    state_t state;

    assign in_ready = (state == S_IDLE);

    // ---------------------------------------------------------------- triangle
    logic signed [15:0] vx [3], vy [3];
    logic [15:0]        va [3][4];      // z, r, g, b
    logic [7:0]         vf [3];
    logic               cfg_cull, cfg_front_cw;   // latched with the triangle

    // ---------------------------------------------------------------- CLASS / AREA
    logic signed [15:0] dx1, dy1, dx2, dy2;
    logic signed [15:0] xmin, xmax, ymin, ymax;
    logic signed [31:0] prod_a, prod_b;

    function automatic logic signed [15:0] min3(input logic signed [15:0] a, b, c);
        automatic logic signed [15:0] m = (a < b) ? a : b;
        return (m < c) ? m : c;
    endfunction
    function automatic logic signed [15:0] max3(input logic signed [15:0] a, b, c);
        automatic logic signed [15:0] m = (a > b) ? a : b;
        return (m > c) ? m : c;
    endfunction

    wire reject = |(vf[0][5:0] & vf[1][5:0] & vf[2][5:0]) ||
                  |((vf[0] | vf[1] | vf[2]) & 8'hC0);          // NEAR or guard band

    // ---------------------------------------------------------------- DECIDE
    wire signed [31:0] area  = prod_a - prod_b;
    wire               front = cfg_front_cw ? (area > 0) : (area < 0);
    wire               cull  = (area == 0) || (cfg_cull && !front);

    // Pixel p is a candidate if its centre 16p+8 lies in [min, max].
    wire signed [11:0] bx0 = 12'((xmin + 16'sd7) >>> 4);
    wire signed [11:0] bx1 = 12'((xmax - 16'sd8) >>> 4);
    wire signed [11:0] by0 = 12'((ymin + 16'sd7) >>> 4);
    wire signed [11:0] by1 = 12'((ymax - 16'sd8) >>> 4);
    wire signed [11:0] cx0 = (bx0 < 0) ? 12'sd0 : bx0;
    wire signed [11:0] cx1 = (bx1 > 12'(SCREEN_W - 1)) ? 12'(SCREEN_W - 1) : bx1;
    wire signed [11:0] cy0 = (by0 < 0) ? 12'sd0 : by0;
    wire signed [11:0] cy1 = (by1 > 12'(SCREEN_H - 1)) ? 12'(SCREEN_H - 1) : by1;
    wire               empty = (cx0 > cx1) || (cy0 > cy1);

    logic [31:0] area_pos;
    logic [9:0]  px0, px1, py0, py1;

    // ---------------------------------------------------------------- PREP
    logic signed [15:0] dxe [3], dye [3];
    logic               tl [3];
    logic signed [16:0] ecy [3], ecx [3];      // first pixel centre minus edge start
    logic signed [16:0] da1 [4], da2 [4];
    logic signed [16:0] dcx, dcy;              // first pixel centre minus v0
    logic [15:0]        amin [4], amax [4];
    logic [4:0]         lz;

    // Count leading zeros (v != 0: zero-area triangles are culled earlier).
    function automatic logic [4:0] clz32(input logic [31:0] v);
        automatic logic [4:0] n = 5'd31;
        for (int b = 0; b < 32; b++)
            if (v[b]) n = 5'(31 - b);
        return n;
    endfunction

    // ---------------------------------------------------------------- reciprocal
    // R = floor(2^55 / An), An = area << clz(area) in [2^31, 2^32) -> R in (2^23, 2^24]
    logic [31:0] d_rem;                       // always < d_den
    logic [31:0] d_den;
    logic [24:0] d_q;
    logic [4:0]  d_cnt;
    logic        d_busy;
    wire  [32:0] d_shift = {d_rem, 1'b0};
    wire         d_ge    = d_shift >= {1'b0, d_den};
    wire  [31:0] d_diff  = d_shift[31:0] - d_den;   // exact when d_ge (result < d_den)

    // ---------------------------------------------------------------- multiplier
    logic [5:0]         op;           // next operation to issue
    logic [5:0]         wb_count;     // operations written back
    logic signed [33:0] num [4][2];   // gradient numerators (x, y)
    logic signed [37:0] grad [4][2];  // gradients per 1/16 px, Q.20
    logic signed [33:0] e0 [3];
    logic signed [37:0] a0 [4];

    logic signed [37:0] mul_a;
    logic signed [25:0] mul_b;
    always_comb begin
        automatic logic [1:0] k;
        mul_a = '0;
        mul_b = '0;
        if (op < 6) begin                              // edge functions
            k = 2'(op >> 1);
            if (!op[0]) begin mul_a = 38'(ecy[k]); mul_b = 26'(dxe[k]); end
            else        begin mul_a = 38'(ecx[k]); mul_b = 26'(dye[k]); end
        end else if (op < 22) begin                    // numerators
            k = 2'((op - 6'd6) >> 2);
            case (op - 6'd6 & 6'd3)
                6'd0:    begin mul_a = 38'(da1[k]); mul_b = 26'(dy2); end
                6'd1:    begin mul_a = 38'(da2[k]); mul_b = 26'(dy1); end
                6'd2:    begin mul_a = 38'(da2[k]); mul_b = 26'(dx1); end
                default: begin mul_a = 38'(da1[k]); mul_b = 26'(dx2); end
            endcase
        end else if (op < 30) begin                    // gradients
            k = 2'((op - 6'd22) >> 1);
            mul_a = 38'(num[k][op[0]]);
            mul_b = $signed({1'b0, d_q});
        end else if (op < NOPS) begin                  // start values
            k = 2'((op - 6'd30) >> 1);
            mul_a = grad[k][op[0]];
            mul_b = op[0] ? 26'(dcy) : 26'(dcx);
        end
    end

    wire ops_ready = (op < 22) ||
                     (op < 30 && wb_count >= 6'd22 && !d_busy) ||
                     (op < NOPS && wb_count >= 6'd30);
    wire issue = (state == S_RUN) && (op < NOPS) && ops_ready;

    // Pipeline: operands -> product -> product reg -> shift -> write-back
    logic signed [37:0] ma_r;
    logic signed [25:0] mb_r;
    logic signed [63:0] m1, m2;
    logic signed [37:0] sh_r;         // every destination is <= 38 bits
    logic [5:0]         t1, t2, t3, t4;
    logic               v1, v2, v3, v4;

    always_ff @(posedge clk) begin
        ma_r <= mul_a;
        mb_r <= mul_b;
        m1   <= ma_r * mb_r;
        m2   <= m1;
        // Gradients: G = (num * R) >>> (35 - lz); others pass through.
        sh_r <= 38'((t3 >= 6'd22 && t3 < 6'd30) ? (m2 >>> (6'd35 - 6'(lz))) : m2);
        t1 <= op;
        t2 <= t1;
        t3 <= t2;
        t4 <= t3;
    end

    // ---------------------------------------------------------------- control
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state     <= S_IDLE;
            culled    <= 1'b0;
            clipped   <= 1'b0;
            out_valid <= 1'b0;
            d_busy    <= 1'b0;
            v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0; v4 <= 1'b0;
        end else begin
            culled  <= 1'b0;
            clipped <= 1'b0;
            if (out_valid && out_ready) out_valid <= 1'b0;

            v1 <= issue;
            v2 <= v1;
            v3 <= v2;
            v4 <= v3;

            if (d_busy && d_cnt == 5'd1) d_busy <= 1'b0;

            case (state)
                S_IDLE:  if (in_valid) state <= S_CLASS;
                S_CLASS: if (reject) begin
                             clipped <= 1'b1;
                             state   <= S_IDLE;
                         end else begin
                             state   <= S_AREA;
                         end
                S_AREA:  state <= S_DECIDE;
                S_DECIDE: if (cull) begin
                             culled <= 1'b1;
                             state  <= S_IDLE;
                         end else if (empty) begin
                             clipped <= 1'b1;
                             state   <= S_IDLE;
                         end else begin
                             state   <= S_PREP;
                         end
                S_PREP: begin
                    d_busy <= 1'b1;
                    state  <= S_RUN;
                end
                S_RUN: if (wb_count == 6'(NOPS)) state <= S_OUT;
                S_OUT: if (!out_valid || out_ready) begin
                    out_valid <= 1'b1;
                    state     <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    // ---------------------------------------------------------------- data path
    always_ff @(posedge clk) begin
        case (state)
            S_IDLE: if (in_valid) begin
                cfg_cull     <= cull_back;
                cfg_front_cw <= front_cw;
                for (int i = 0; i < 3; i++) begin
                    vx[i]    <= in_sx[i];
                    vy[i]    <= in_sy[i];
                    va[i][0] <= in_z[i];
                    va[i][1] <= {8'd0, in_color[i][23:16]};
                    va[i][2] <= {8'd0, in_color[i][15:8]};
                    va[i][3] <= {8'd0, in_color[i][7:0]};
                    vf[i]    <= in_flags[i];
                end
            end
            S_CLASS: begin
                dx1  <= vx[1] - vx[0];
                dy1  <= vy[1] - vy[0];
                dx2  <= vx[2] - vx[0];
                dy2  <= vy[2] - vy[0];
                xmin <= min3(vx[0], vx[1], vx[2]);
                xmax <= max3(vx[0], vx[1], vx[2]);
                ymin <= min3(vy[0], vy[1], vy[2]);
                ymax <= max3(vy[0], vy[1], vy[2]);
            end
            S_AREA: begin
                prod_a <= dx1 * dy2;
                prod_b <= dx2 * dy1;
            end
            S_DECIDE: begin
                px0 <= 10'(cx0);
                px1 <= 10'(cx1);
                py0 <= 10'(cy0);
                py1 <= 10'(cy1);
                area_pos <= (area < 0) ? 32'(-area) : 32'(area);
                if (area < 0) begin   // make the winding positive
                    vx[1] <= vx[2];  vx[2] <= vx[1];
                    vy[1] <= vy[2];  vy[2] <= vy[1];
                    va[1] <= va[2];  va[2] <= va[1];
                    dx1 <= dx2;  dx2 <= dx1;
                    dy1 <= dy2;  dy2 <= dy1;
                end
            end
            S_PREP: begin
                for (int e = 0; e < 3; e++) begin
                    automatic logic [1:0] n = 2'((e + 1) % 3);
                    automatic logic signed [15:0] ddx = vx[n] - vx[e];
                    automatic logic signed [15:0] ddy = vy[n] - vy[e];
                    dxe[e] <= ddx;
                    dye[e] <= ddy;
                    // Top-left rule: left edges (dy < 0) and top edges
                    // (dy == 0, dx > 0) own the pixels exactly on them.
                    tl[e]  <= (ddy < 0) || (ddy == 0 && ddx > 0);
                    ecy[e] <= 17'({py0, 4'd8}) - 17'(vy[e]);
                    ecx[e] <= 17'({px0, 4'd8}) - 17'(vx[e]);
                end
                for (int i = 0; i < 4; i++) begin
                    da1[i] <= 17'(va[1][i]) - 17'(va[0][i]);
                    da2[i] <= 17'(va[2][i]) - 17'(va[0][i]);
                    amin[i] <= (va[0][i] < va[1][i]) ? ((va[0][i] < va[2][i]) ? va[0][i] : va[2][i])
                                                     : ((va[1][i] < va[2][i]) ? va[1][i] : va[2][i]);
                    amax[i] <= (va[0][i] > va[1][i]) ? ((va[0][i] > va[2][i]) ? va[0][i] : va[2][i])
                                                     : ((va[1][i] > va[2][i]) ? va[1][i] : va[2][i]);
                    a0[i] <= 38'(va[0][i]) <<< 20;
                end
                dcx <= 17'({px0, 4'd8}) - 17'(vx[0]);
                dcy <= 17'({py0, 4'd8}) - 17'(vy[0]);
                lz    <= clz32(area_pos);
                d_den <= area_pos << clz32(area_pos);
                d_rem <= 32'd1 << 30;
                d_q   <= '0;
                d_cnt <= 5'd25;
                op       <= '0;
                wb_count <= '0;
            end
            S_OUT: if (!out_valid || out_ready) begin
                out_px0 <= px0;
                out_px1 <= px1;
                out_py0 <= py0;
                out_py1 <= py1;
                for (int e = 0; e < 3; e++) begin
                    out_e0[e] <= e0[e];
                    out_ex[e] <= -21'(dye[e]) <<< 4;
                    out_ey[e] <= 21'(dxe[e]) <<< 4;
                end
                for (int i = 0; i < 4; i++) begin
                    out_a0[i]   <= a0[i];
                    out_ax[i]   <= grad[i][0] <<< 4;
                    out_ay[i]   <= grad[i][1] <<< 4;
                    out_amin[i] <= amin[i];
                    out_amax[i] <= amax[i];
                end
            end
            default: ;
        endcase

        // Reciprocal iterations
        if (d_busy) begin
            d_rem <= d_ge ? d_diff : d_shift[31:0];
            d_q   <= {d_q[23:0], d_ge};
            d_cnt <= d_cnt - 1'b1;
        end

        // Issue and write-back
        if (issue) op <= op + 1'b1;
        if (v4) begin
            automatic logic [1:0] k;
            wb_count <= wb_count + 1'b1;
            if (t4 < 6) begin
                k = 2'(t4 >> 1);
                if (!t4[0]) e0[k] <= 34'(sh_r);
                else        e0[k] <= e0[k] - 34'(sh_r) - (tl[k] ? 34'sd0 : 34'sd1);
            end else if (t4 < 22) begin
                k = 2'((t4 - 6'd6) >> 2);
                case (t4 - 6'd6 & 6'd3)
                    6'd0:    num[k][0] <= 34'(sh_r);
                    6'd1:    num[k][0] <= num[k][0] - 34'(sh_r);
                    6'd2:    num[k][1] <= 34'(sh_r);
                    default: num[k][1] <= num[k][1] - 34'(sh_r);
                endcase
            end else if (t4 < 30) begin
                k = 2'((t4 - 6'd22) >> 1);
                grad[k][t4[0]] <= 38'(sh_r);
            end else begin
                k = 2'((t4 - 6'd30) >> 1);
                a0[k] <= a0[k] + sh_r;
            end
        end
    end

    assign idle = (state == S_IDLE) && !out_valid;

endmodule

`default_nettype wire
