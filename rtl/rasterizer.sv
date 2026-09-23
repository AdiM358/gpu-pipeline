`default_nettype none

// Span rasterizer with incremental edge functions and attribute stepping
// (bit-exact with model::rasterize in model/gpu_model.cpp).
//
// Stage T (traverse): walks the bounding box row by row, SPAN pixels at a
//   time. For the current span it evaluates all three edge functions at all
//   SPAN pixel centres (E + k*dE/dx, k = 0..SPAN-1) and forms a coverage mask.
//   An empty span costs one cycle; a span with coverage is handed to stage S.
// Stage S (serialize): emits the covered pixels of one span, one per cycle,
//   lowest x first, computing each pixel's attributes as span value + k*step,
//   rounding and clamping them.
// Stage O: output register (valid/ready).
//
// The ROP accepts one fragment per cycle, so there is nothing to gain from
// emitting more; SPAN > 1 is about not spending cycles on empty pixels.
// SPAN = 1 degenerates to the classic one-pixel-per-cycle bounding-box walk.
//
// Edge functions and attributes are only ever added to, never multiplied, and
// the attribute accumulators wrap at 38 bits: modular arithmetic makes the
// value at every covered pixel exact regardless of intermediate overflow.
module rasterizer #(
    parameter int SPAN = 4        // 1, 2, 4 or 8
)(
    input  wire                clk,
    input  wire                rst_n,

    input  wire        [9:0]   in_px0, in_px1, in_py0, in_py1,
    input  wire signed [33:0]  in_e0 [3],
    input  wire signed [20:0]  in_ex [3],
    input  wire signed [20:0]  in_ey [3],
    input  wire signed [37:0]  in_a0 [4],
    input  wire signed [37:0]  in_ax [4],
    input  wire signed [37:0]  in_ay [4],
    input  wire        [15:0]  in_amin [4],
    input  wire        [15:0]  in_amax [4],
    input  wire                in_valid,
    output logic               in_ready,

    output logic       [9:0]   frag_x, frag_y,
    output logic       [15:0]  frag_z,
    output logic       [7:0]   frag_r, frag_g, frag_b,
    output logic               frag_valid,
    input  wire                frag_ready,

    output logic               busy,      // a triangle is being rasterized
    output logic               idle
);

    localparam int KW = (SPAN > 1) ? $clog2(SPAN) : 1;

    // ---------------------------------------------------------------- triangle constants
    logic [9:0]         px0, px1, py1;
    logic signed [33:0] exk [3][SPAN];   // k * dE/dx
    logic signed [33:0] exs [3];         // SPAN * dE/dx
    logic signed [33:0] ey [3];
    logic signed [37:0] axk [4][SPAN];   // k * dA/dx
    logic signed [37:0] axs [4];         // SPAN * dA/dx
    logic signed [37:0] ay [4];
    logic [15:0]        amin [4], amax [4];

    // ---------------------------------------------------------------- stage T
    logic               t_active;
    logic [9:0]         t_x, t_y;
    logic signed [33:0] e_cur [3], e_row [3];
    logic signed [37:0] a_cur [4], a_row [4];

    logic [SPAN-1:0] mask;
    always_comb begin
        for (int k = 0; k < SPAN; k++) begin
            mask[k] = (11'(t_x) + 11'(k) <= 11'(px1));
            for (int e = 0; e < 3; e++)
                mask[k] = mask[k] && ((e_cur[e] + exk[e][k]) >= 0);
        end
    end

    wire last_span = 11'(t_x) + 11'(SPAN) > 11'(px1);
    wire last_row  = (t_y == py1);

    // ---------------------------------------------------------------- stage S
    logic               s_valid;
    logic [SPAN-1:0]    s_mask;
    logic [9:0]         s_x, s_y;
    logic signed [37:0] s_a [4];

    // Lowest set bit of the mask and the mask without it.
    logic [KW-1:0]   s_k;
    logic [SPAN-1:0] s_rest;
    always_comb begin
        s_k = '0;
        for (int k = SPAN - 1; k >= 0; k--)
            if (s_mask[k]) s_k = KW'(k);
        s_rest = s_mask & (s_mask - 1'b1);
    end

    wire o_free    = !frag_valid || frag_ready;
    wire s_emit    = s_valid && o_free;
    wire s_free    = !s_valid || (s_emit && s_rest == '0);
    // T advances past an empty span even while S is still emitting pixels of
    // an earlier span; only a covered span has to wait for S to be free.
    wire t_step    = t_active && (mask == '0 || s_free);
    wire t_handoff = t_step && (mask != '0);

    // A new triangle may load only when stage S has drained, because S reads
    // the per-triangle step tables.
    assign in_ready = !t_active && !s_valid;
    wire   load     = in_valid && in_ready;

    // ---------------------------------------------------------------- attribute output
    function automatic logic [15:0] finish_attr(input logic signed [37:0] acc,
                                                input logic [15:0] lo, input logic [15:0] hi);
        // Round to nearest (38-bit wrap), drop 20 fraction bits, clamp to the
        // range spanned by the triangle's vertices.
        automatic logic signed [37:0] r = acc + 38'sd524288;
        automatic logic signed [17:0] v = 18'(r >>> 20);
        if (v < $signed({2'b00, lo})) return lo;
        if (v > $signed({2'b00, hi})) return hi;
        return v[15:0];
    endfunction

    // ---------------------------------------------------------------- control
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            t_active   <= 1'b0;
            s_valid    <= 1'b0;
            frag_valid <= 1'b0;
        end else begin
            if (load) t_active <= 1'b1;
            else if (t_step && last_span && last_row) t_active <= 1'b0;

            if (t_handoff)   s_valid <= 1'b1;
            else if (s_free) s_valid <= 1'b0;

            if (s_emit)          frag_valid <= 1'b1;
            else if (frag_ready) frag_valid <= 1'b0;
        end
    end

    // ---------------------------------------------------------------- data path
    always_ff @(posedge clk) begin
        if (load) begin
            px0 <= in_px0;
            px1 <= in_px1;
            py1 <= in_py1;
            t_x <= in_px0;
            t_y <= in_py0;
            for (int e = 0; e < 3; e++) begin
                e_cur[e] <= in_e0[e];
                e_row[e] <= in_e0[e];
                ey[e]    <= 34'(in_ey[e]);
                exs[e]   <= 34'(in_ex[e]) * 34'(SPAN);
                for (int k = 0; k < SPAN; k++) exk[e][k] <= 34'(in_ex[e]) * 34'(k);
            end
            for (int i = 0; i < 4; i++) begin
                a_cur[i] <= in_a0[i];
                a_row[i] <= in_a0[i];
                ay[i]    <= in_ay[i];
                axs[i]   <= in_ax[i] * 38'(SPAN);
                for (int k = 0; k < SPAN; k++) axk[i][k] <= in_ax[i] * 38'(k);
                amin[i]  <= in_amin[i];
                amax[i]  <= in_amax[i];
            end
        end else if (t_step) begin
            if (!last_span) begin
                t_x <= t_x + 10'(SPAN);
                for (int e = 0; e < 3; e++) e_cur[e] <= e_cur[e] + exs[e];
                for (int i = 0; i < 4; i++) a_cur[i] <= a_cur[i] + axs[i];
            end else begin
                t_x <= px0;
                t_y <= t_y + 1'b1;
                for (int e = 0; e < 3; e++) begin
                    e_row[e] <= e_row[e] + ey[e];
                    e_cur[e] <= e_row[e] + ey[e];
                end
                for (int i = 0; i < 4; i++) begin
                    a_row[i] <= a_row[i] + ay[i];
                    a_cur[i] <= a_row[i] + ay[i];
                end
            end
        end

        if (t_handoff) begin
            s_mask <= mask;
            s_x    <= t_x;
            s_y    <= t_y;
            s_a    <= a_cur;
        end else if (s_emit) begin
            s_mask <= s_rest;
        end

        if (s_emit) begin
            frag_x <= s_x + 10'(s_k);
            frag_y <= s_y;
            frag_z <= finish_attr(s_a[0] + axk[0][s_k], amin[0], amax[0]);
            frag_r <= 8'(finish_attr(s_a[1] + axk[1][s_k], amin[1], amax[1]));
            frag_g <= 8'(finish_attr(s_a[2] + axk[2][s_k], amin[2], amax[2]));
            frag_b <= 8'(finish_attr(s_a[3] + axk[3][s_k], amin[3], amax[3]));
        end
    end

    assign busy = t_active || s_valid;
    assign idle = !t_active && !s_valid && !frag_valid;

endmodule

`default_nettype wire
