`default_nettype none

// Perspective divide and viewport transform (bit-exact with
// model::persp_viewport in model/gpu_model.cpp).
//
//   F  flags: clip-space outcodes (x,y,z vs +/-w), NEAR (w <= 1/16)
//   D  recip = floor(2^44 / w)     32-stage pipelined divider, 1/cycle
//   M1 n = clip * recip            3 multiplies instead of 3 divides
//   M2 product register
//   V  NDC (Q.20) -> Q11.4 screen coordinates, guard-band flag, 16-bit depth
//
// Screen: sx = 8W*(1 + x/w), sy = 8H*(1 - y/w) in 1/16 px (y points down);
// depth = clamp((z/w + 1) * 65535/2, 0, 65535).
module persp_viewport #(
    parameter int SCREEN_W = 320,
    parameter int SCREEN_H = 240
)(
    input  wire                clk,
    input  wire                rst_n,

    input  wire signed [31:0]  in_x, in_y, in_z, in_w,   // Q16.16 clip space
    input  wire        [23:0]  in_color,
    input  wire                in_valid,
    output logic               in_ready,

    output logic signed [15:0] out_sx, out_sy,           // Q11.4
    output logic       [15:0]  out_z,
    output logic       [23:0]  out_color,
    output logic       [7:0]   out_flags,                // {GB, NEAR, OC[5:0]}
    output logic               out_valid,
    input  wire                out_ready,

    output logic               idle
);

    localparam logic signed [31:0] W_NEAR = 32'sh0000_1000;  // 1/16
    localparam logic signed [63:0] HW = 64'(8 * SCREEN_W);     // half width, 1/16 px
    localparam logic signed [63:0] HH = 64'(8 * SCREEN_H);
    localparam logic signed [63:0] GUARD = 64'sd16384;

    wire en = !out_valid || out_ready;
    assign in_ready = en;

    // ---------------------------------------------------------------- F
    logic               vld_f;
    logic signed [31:0] fx, fy, fz, fw;
    logic [23:0]        fcol;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)  vld_f <= 1'b0;
        else if (en) vld_f <= in_valid;
    end
    always_ff @(posedge clk) begin
        if (en) begin
            fx   <= in_x;
            fy   <= in_y;
            fz   <= in_z;
            fw   <= in_w;
            fcol <= in_color;
        end
    end

    // Outcodes compare against +/-w with one extra bit so -w cannot overflow.
    wire signed [32:0] w33  = 33'(fw);
    wire signed [32:0] nw33 = -w33;
    wire [5:0] oc = {33'(fz) > w33, 33'(fz) < nw33,
                     33'(fy) > w33, 33'(fy) < nw33,
                     33'(fx) > w33, 33'(fx) < nw33};
    wire near = fw <= W_NEAR;

    // ---------------------------------------------------------------- D
    localparam int PAY_W = 96 + 24 + 7;
    logic [PAY_W-1:0] d_pay;
    logic [31:0]      recip;
    logic             vld_d;

    recip_pipe #(.PAY_W(PAY_W)) u_recip (
        .clk       (clk),
        .rst_n     (rst_n),
        .en        (en),
        .in_valid  (vld_f),
        .in_d      (near ? 31'h2000 : fw[30:0]),  // near: harmless divisor, result unused
        .in_pay    ({near, oc, fcol, fz, fy, fx}),
        .out_valid (vld_d),
        .out_q     (recip),
        .out_pay   (d_pay)
    );

    wire signed [31:0] dx = d_pay[31:0];
    wire signed [31:0] dy = d_pay[63:32];
    wire signed [31:0] dz = d_pay[95:64];

    // ---------------------------------------------------------------- M1, M2
    logic               vld_m1, vld_m2;
    logic signed [63:0] px1, py1, pz1, px2, py2, pz2;
    logic [30:0]        m1_meta, m2_meta;   // {near, oc, color}

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            vld_m1 <= 1'b0;
            vld_m2 <= 1'b0;
        end else if (en) begin
            vld_m1 <= vld_d;
            vld_m2 <= vld_m1;
        end
    end
    always_ff @(posedge clk) begin
        if (en) begin
            // signed 32 x unsigned 32: the product magnitude is < 2^63
            px1     <= dx * $signed({1'b0, recip});
            py1     <= dy * $signed({1'b0, recip});
            pz1     <= dz * $signed({1'b0, recip});
            m1_meta <= d_pay[PAY_W-1:96];
            px2     <= px1;
            py2     <= py1;
            pz2     <= pz1;
            m2_meta <= m1_meta;
        end
    end

    // ---------------------------------------------------------------- V
    wire signed [39:0] nx = 40'(px2 >>> 24);   // Q.20 NDC
    wire signed [39:0] ny = 40'(py2 >>> 24);
    wire signed [39:0] nz = 40'(pz2 >>> 24);

    wire signed [63:0] sx_full = (64'(nx) * HW + (HW <<< 20)) >>> 20;
    wire signed [63:0] sy_full = ((HH <<< 20) - 64'(ny) * HH) >>> 20;
    wire               gb = sx_full < -GUARD || sx_full >= GUARD ||
                            sy_full < -GUARD || sy_full >= GUARD;

    wire signed [63:0] depth_full = ((64'(nz) + 64'sd1048576) * 64'sd65535) >>> 21;
    wire [15:0] depth = depth_full < 0 ? 16'd0 :
                        depth_full > 64'sd65535 ? 16'hFFFF : depth_full[15:0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)  out_valid <= 1'b0;
        else if (en) out_valid <= vld_m2;
    end
    always_ff @(posedge clk) begin
        if (en) begin
            out_sx    <= sx_full[15:0];
            out_sy    <= sy_full[15:0];
            out_z     <= depth;
            out_color <= m2_meta[23:0];
            out_flags <= {gb, m2_meta[30], m2_meta[29:24]};
        end
    end

    // Idle: every accepted vertex has been delivered. A counter is simpler
    // than OR-ing the valid bits of all 36 stages. Max occupancy is 36 < 128.
    logic [6:0] in_flight;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) in_flight <= '0;
        else in_flight <= in_flight + 7'(in_valid && in_ready) - 7'(out_valid && out_ready);
    end
    assign idle = (in_flight == 0);

endmodule

`default_nettype wire
