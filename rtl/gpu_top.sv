`default_nettype none

// GPU top level.
//
//   AXI-Lite regs -> gpu_ctrl
//   vertex_fetch -> geom_engine -> persp_viewport -> prim_assembly
//     -> tri_setup -> rasterizer -> rop (depth test, colour + depth buffers)
//
// The colour and depth buffers are on chip (SCREEN_W x SCREEN_H x 16 bit
// each) and readable through the fb_rd_* port (2-cycle latency).
module gpu_top #(
    parameter int SCREEN_W  = 320,
    parameter int SCREEN_H  = 240,
    parameter int RAST_SPAN = 4,
    localparam int FB_AW    = $clog2(SCREEN_W * SCREEN_H)
)(
    input  wire         clk,
    input  wire         rst_n,

    // AXI4-Lite slave: register file (docs/REGMAP.md)
    input  wire  [7:0]  s_axi_awaddr,
    input  wire         s_axi_awvalid,
    output logic        s_axi_awready,
    input  wire  [31:0] s_axi_wdata,
    input  wire  [3:0]  s_axi_wstrb,
    input  wire         s_axi_wvalid,
    output logic        s_axi_wready,
    output logic [1:0]  s_axi_bresp,
    output logic        s_axi_bvalid,
    input  wire         s_axi_bready,
    input  wire  [7:0]  s_axi_araddr,
    input  wire         s_axi_arvalid,
    output logic        s_axi_arready,
    output logic [31:0] s_axi_rdata,
    output logic [1:0]  s_axi_rresp,
    output logic        s_axi_rvalid,
    input  wire         s_axi_rready,
    output logic        irq,

    // AXI4 read master: vertex buffer
    output logic [31:0] m_axi_araddr,
    output logic [7:0]  m_axi_arlen,
    output logic [2:0]  m_axi_arsize,
    output logic [1:0]  m_axi_arburst,
    output logic        m_axi_arvalid,
    input  wire         m_axi_arready,
    input  wire  [31:0] m_axi_rdata,
    input  wire  [1:0]  m_axi_rresp,
    input  wire         m_axi_rlast,
    input  wire         m_axi_rvalid,
    output logic        m_axi_rready,

    // Framebuffer read port (scan-out / readback), 2-cycle latency.
    // fb_rd_depth selects the depth buffer (valid only while not busy).
    input  wire  [FB_AW-1:0] fb_rd_addr,
    input  wire              fb_rd_depth,
    output logic [15:0]      fb_rd_data
);

    // ================================================================ control
    logic        cmd_draw, cmd_clear, cmd_start, done_set, busy;
    logic        start_draw, start_clear, clear_busy;
    logic [31:0] vbuf_base, vertex_count;
    logic        cull_back, front_cw;
    logic [15:0] clear_color;
    logic signed [31:0] mvp [4][4];
    logic [31:0] perf [8];
    logic        fetch_busy, fetch_err;
    logic        pipe_idle;

    axil_regs #(.ADDR_W(8), .FB_W(SCREEN_W), .FB_H(SCREEN_H)) u_regs (
        .clk, .rst_n,
        .s_axi_awaddr, .s_axi_awvalid, .s_axi_awready,
        .s_axi_wdata, .s_axi_wstrb, .s_axi_wvalid, .s_axi_wready,
        .s_axi_bresp, .s_axi_bvalid, .s_axi_bready,
        .s_axi_araddr, .s_axi_arvalid, .s_axi_arready,
        .s_axi_rdata, .s_axi_rresp, .s_axi_rvalid, .s_axi_rready,
        .cmd_draw, .cmd_clear,
        .vbuf_base, .vertex_count, .cull_back_en(cull_back), .front_cw,
        .clear_color, .mvp, .irq,
        .busy, .done_set, .err_set(fetch_err), .perf
    );

    // Every stage's idle output is exact (it covers the stage's own
    // registers), so a single idle cycle after fetch finishes is enough.
    gpu_ctrl #(.DRAIN_CYCLES(1)) u_ctrl (
        .clk, .rst_n,
        .cmd_draw, .cmd_clear,
        .start_clear, .clear_busy,
        .start_draw, .fetch_busy, .pipe_idle,
        .busy, .cmd_start, .done_set
    );

    // ================================================================ geometry
    logic signed [31:0] f_x, f_y, f_z;
    logic [23:0]        f_color;
    logic               f_valid, f_ready;

    vertex_fetch u_fetch (
        .clk, .rst_n,
        .start(start_draw), .base_addr(vbuf_base), .vertex_count,
        .busy(fetch_busy), .error_set(fetch_err),
        .m_axi_araddr, .m_axi_arlen, .m_axi_arsize, .m_axi_arburst,
        .m_axi_arvalid, .m_axi_arready,
        .m_axi_rdata, .m_axi_rresp, .m_axi_rlast, .m_axi_rvalid, .m_axi_rready,
        .out_x(f_x), .out_y(f_y), .out_z(f_z), .out_color(f_color),
        .out_valid(f_valid), .out_ready(f_ready)
    );

    logic signed [31:0] c_x, c_y, c_z, c_w;
    logic [23:0]        c_color;
    logic               c_valid, c_ready, geom_idle;

    geom_engine u_geom (
        .clk, .rst_n, .mvp,
        .in_x(f_x), .in_y(f_y), .in_z(f_z), .in_color(f_color),
        .in_valid(f_valid), .in_ready(f_ready),
        .out_x(c_x), .out_y(c_y), .out_z(c_z), .out_w(c_w), .out_color(c_color),
        .out_valid(c_valid), .out_ready(c_ready), .idle(geom_idle)
    );

    logic signed [15:0] s_sx, s_sy;
    logic [15:0]        s_z;
    logic [23:0]        s_color;
    logic [7:0]         s_flags;
    logic               s_valid, s_ready, persp_idle;

    persp_viewport #(.SCREEN_W(SCREEN_W), .SCREEN_H(SCREEN_H)) u_persp (
        .clk, .rst_n,
        .in_x(c_x), .in_y(c_y), .in_z(c_z), .in_w(c_w), .in_color(c_color),
        .in_valid(c_valid), .in_ready(c_ready),
        .out_sx(s_sx), .out_sy(s_sy), .out_z(s_z), .out_color(s_color), .out_flags(s_flags),
        .out_valid(s_valid), .out_ready(s_ready), .idle(persp_idle)
    );

    logic signed [15:0] t_sx [3], t_sy [3];
    logic [15:0]        t_z [3];
    logic [23:0]        t_color [3];
    logic [7:0]         t_flags [3];
    logic               t_valid, t_ready, prim_idle;

    prim_assembly u_prim (
        .clk, .rst_n, .flush(start_draw),
        .in_sx(s_sx), .in_sy(s_sy), .in_z(s_z), .in_color(s_color), .in_flags(s_flags),
        .in_valid(s_valid), .in_ready(s_ready),
        .out_sx(t_sx), .out_sy(t_sy), .out_z(t_z), .out_color(t_color), .out_flags(t_flags),
        .out_valid(t_valid), .out_ready(t_ready), .idle(prim_idle)
    );

    // ================================================================ raster
    logic [9:0]         r_px0, r_px1, r_py0, r_py1;
    logic signed [33:0] r_e0 [3];
    logic signed [20:0] r_ex [3], r_ey [3];
    logic signed [37:0] r_a0 [4], r_ax [4], r_ay [4];
    logic [15:0]        r_amin [4], r_amax [4];
    logic               r_valid, r_ready, setup_idle, tri_culled, tri_clipped;

    tri_setup #(.SCREEN_W(SCREEN_W), .SCREEN_H(SCREEN_H)) u_setup (
        .clk, .rst_n, .cull_back, .front_cw,
        .in_sx(t_sx), .in_sy(t_sy), .in_z(t_z), .in_color(t_color), .in_flags(t_flags),
        .in_valid(t_valid), .in_ready(t_ready),
        .out_px0(r_px0), .out_px1(r_px1), .out_py0(r_py0), .out_py1(r_py1),
        .out_e0(r_e0), .out_ex(r_ex), .out_ey(r_ey),
        .out_a0(r_a0), .out_ax(r_ax), .out_ay(r_ay), .out_amin(r_amin), .out_amax(r_amax),
        .out_valid(r_valid), .out_ready(r_ready),
        .culled(tri_culled), .clipped(tri_clipped), .idle(setup_idle)
    );

    logic [9:0]  fr_x, fr_y;
    logic [15:0] fr_z;
    logic [7:0]  fr_r, fr_g, fr_b;
    logic        fr_valid, fr_ready, rast_busy, rast_idle;

    rasterizer #(.SPAN(RAST_SPAN)) u_rast (
        .clk, .rst_n,
        .in_px0(r_px0), .in_px1(r_px1), .in_py0(r_py0), .in_py1(r_py1),
        .in_e0(r_e0), .in_ex(r_ex), .in_ey(r_ey),
        .in_a0(r_a0), .in_ax(r_ax), .in_ay(r_ay), .in_amin(r_amin), .in_amax(r_amax),
        .in_valid(r_valid), .in_ready(r_ready),
        .frag_x(fr_x), .frag_y(fr_y), .frag_z(fr_z), .frag_r(fr_r), .frag_g(fr_g), .frag_b(fr_b),
        .frag_valid(fr_valid), .frag_ready(fr_ready),
        .busy(rast_busy), .idle(rast_idle)
    );

    // ================================================================ back end
    logic rop_idle, frag_pass;

    rop #(.SCREEN_W(SCREEN_W), .SCREEN_H(SCREEN_H)) u_rop (
        .clk, .rst_n,
        .in_x(fr_x), .in_y(fr_y), .in_z(fr_z), .in_r(fr_r), .in_g(fr_g), .in_b(fr_b),
        .in_valid(fr_valid), .in_ready(fr_ready),
        .start_clear, .clear_color, .clear_busy,
        .ext_addr(fb_rd_addr), .ext_depth(fb_rd_depth), .ext_data(fb_rd_data),
        .frag_pass, .idle(rop_idle)
    );

    // ================================================================ status
    assign pipe_idle = geom_idle && persp_idle && prim_idle && setup_idle && rast_idle && rop_idle;

    // Performance counters (order matches GPU_REG_PERF_* in sw/gpu_regs.h)
    wire [7:0] perf_inc = {
        rast_busy,               // 7 rasterizer busy cycles
        frag_pass,               // 6 fragments passing the depth test
        fr_valid && fr_ready,    // 5 fragments generated
        tri_clipped,             // 4 triangles rejected
        tri_culled,              // 3 triangles culled
        t_valid && t_ready,      // 2 triangles assembled
        f_valid && f_ready,      // 1 vertices fetched
        busy                     // 0 command cycles
    };

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 8; i++) perf[i] <= '0;
        end else begin
            for (int i = 0; i < 8; i++) begin
                if (cmd_start)        perf[i] <= '0;
                else if (perf_inc[i]) perf[i] <= perf[i] + 1'b1;
            end
        end
    end

endmodule

`default_nettype wire
