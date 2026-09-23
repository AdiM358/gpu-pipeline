`default_nettype none

module gpu_top #(
    parameter integer AXI_ADDR_WIDTH = 32,     // AXI4 master (vertex fetch)
    parameter integer AXI_DATA_WIDTH = 32,
    parameter integer FRAC_BITS      = 16,
    parameter integer SCREEN_W       = 640,
    parameter integer SCREEN_H       = 480
)(
    input wire clk,
    input wire rst_n,

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

    // AXI4 Master Interface (VRAM Fetch)
    output logic [AXI_ADDR_WIDTH-1:0] m_axi_araddr,
    output logic [7:0] m_axi_arlen,
    output logic [2:0] m_axi_arsize,
    output logic [1:0] m_axi_arburst,
    output logic m_axi_arvalid,
    input wire m_axi_arready,

    input wire [AXI_DATA_WIDTH-1:0] m_axi_rdata,
    input wire [1:0] m_axi_rresp,
    input wire m_axi_rlast,
    input wire m_axi_rvalid,
    output logic m_axi_rready,

    // Memory Interface: Z-Buffer (Read)
    output logic [31:0]      m_zbuf_rd_addr,
    output logic             m_zbuf_rd_en,
    input wire [31:0]        s_zbuf_rd_data,

    // Memory Interface: Z-Buffer (Write)
    output logic [31:0]      m_zbuf_wr_addr,
    output logic signed [31:0] m_zbuf_wr_data,
    output logic             m_zbuf_wr_en,

    // Memory Interface: Framebuffer (Write)
    output logic [31:0]      m_fb_wr_addr,
    output logic [31:0]      m_fb_wr_data,
    output logic             m_fb_wr_en
);

    // Register file / controller
    logic        cmd_draw, cmd_clear, cmd_start, done_set;
    logic        start_pulse;
    logic        busy;
    logic [31:0] vbuf_base_addr;
    logic [31:0] vertex_count;
    /* verilator lint_off UNUSEDSIGNAL */
    logic        cull_back_en, front_cw;   // consumed once triangle setup exists
    logic [15:0] clear_color;
    logic        start_clear;              // no clear engine in the baseline datapath
    /* verilator lint_on UNUSEDSIGNAL */
    logic        fetch_busy, fetch_err;
    logic signed [31:0] mvp_matrix [4][4];
    logic [31:0] perf [8];

    // Fetch -> Geom interconnect
    logic signed [31:0] fetch_vx, fetch_vy, fetch_vz;
    logic [23:0] fetch_color;
    logic fetch_valid, fetch_ready;

    // Geom -> Persp interconnect
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [31:0] clip_x /* verilator public */;
    logic signed [31:0] clip_y /* verilator public */;
    logic signed [31:0] clip_z /* verilator public */;
    logic signed [31:0] clip_w /* verilator public */;

    logic [23:0]        clip_color /* verilator public */;
    logic               clip_valid /* verilator public */;
    logic               clip_ready /* verilator public */;
    /* verilator lint_on UNUSEDSIGNAL */


    logic geom_idle, persp_idle;
    logic signed [15:0] scr_sx, scr_sy;
    logic [15:0] scr_z;
    logic [23:0] scr_color;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [7:0]  scr_flags;   // consumed by triangle setup (next phase)
    /* verilator lint_on UNUSEDSIGNAL */

    // Persp -> Prim Assembly interconnect
    logic signed [31:0] screen_x, screen_y, screen_z;
    logic [31:0] screen_color;
    logic screen_valid, screen_ready;

    // Prim Assembly -> Rasterizer interconnect
    logic signed [31:0] tri_v0_x /* verilator public */;
    logic signed [31:0] tri_v0_y /* verilator public */;
    logic signed [31:0] tri_v0_z /* verilator public */;
    
    logic signed [31:0] tri_v1_x /* verilator public */;
    logic signed [31:0] tri_v1_y /* verilator public */;
    logic signed [31:0] tri_v1_z /* verilator public */;
    
    logic signed [31:0] tri_v2_x /* verilator public */;
    logic signed [31:0] tri_v2_y /* verilator public */;
    logic signed [31:0] tri_v2_z /* verilator public */;
    
    logic [31:0]        tri_color /* verilator public */;
    
    logic               tri_valid /* verilator public */;
    logic               tri_ready /* verilator public */;

    // Rasterizer -> Pixel Map interconnect
    logic signed [15:0] frag_x, frag_y;
    logic signed [31:0] frag_z;
    logic [31:0]        frag_color;
    logic               frag_valid, frag_ready;

    axil_regs #(
        .ADDR_W (8),
        .FB_W   (SCREEN_W),
        .FB_H   (SCREEN_H)
    ) u_regs (
        .clk           (clk),
        .rst_n         (rst_n),
        .s_axi_awaddr  (s_axi_awaddr),
        .s_axi_awvalid (s_axi_awvalid),
        .s_axi_awready (s_axi_awready),
        .s_axi_wdata   (s_axi_wdata),
        .s_axi_wstrb   (s_axi_wstrb),
        .s_axi_wvalid  (s_axi_wvalid),
        .s_axi_wready  (s_axi_wready),
        .s_axi_bresp   (s_axi_bresp),
        .s_axi_bvalid  (s_axi_bvalid),
        .s_axi_bready  (s_axi_bready),
        .s_axi_araddr  (s_axi_araddr),
        .s_axi_arvalid (s_axi_arvalid),
        .s_axi_arready (s_axi_arready),
        .s_axi_rdata   (s_axi_rdata),
        .s_axi_rresp   (s_axi_rresp),
        .s_axi_rvalid  (s_axi_rvalid),
        .s_axi_rready  (s_axi_rready),
        .cmd_draw      (cmd_draw),
        .cmd_clear     (cmd_clear),
        .vbuf_base     (vbuf_base_addr),
        .vertex_count  (vertex_count),
        .cull_back_en  (cull_back_en),
        .front_cw      (front_cw),
        .clear_color   (clear_color),
        .mvp           (mvp_matrix),
        .irq           (irq),
        .busy          (busy),
        .done_set      (done_set),
        .err_set       (fetch_err),
        .perf          (perf)
    );

    // Interim drain detection for the baseline datapath, which exposes no
    // idle signals: every inter-stage valid is low and the rasterizer and
    // pixel map are waiting for input. gpu_ctrl additionally requires this to
    // hold for several consecutive cycles to cover the stages' internal
    // pipeline registers.
    wire pipe_idle = geom_idle && persp_idle && !tri_valid &&
                     tri_ready && !frag_valid && frag_ready;

    gpu_ctrl #(
        .DRAIN_CYCLES (4)
    ) u_ctrl (
        .clk         (clk),
        .rst_n       (rst_n),
        .cmd_draw    (cmd_draw),
        .cmd_clear   (cmd_clear),
        .start_clear (start_clear),
        .clear_busy  (1'b0),
        .start_draw  (start_pulse),
        .fetch_busy  (fetch_busy),
        .pipe_idle   (pipe_idle),
        .busy        (busy),
        .cmd_start   (cmd_start),
        .done_set    (done_set)
    );

    // Performance counters (order matches GPU_REG_PERF_* in sw/gpu_regs.h)
    wire [7:0] perf_inc = {
        !tri_ready,                  // 7 rasterizer busy cycles
        m_fb_wr_en,                  // 6 fragments passing depth test
        frag_valid && frag_ready,    // 5 fragments generated
        1'b0,                        // 4 triangles clipped
        1'b0,                        // 3 triangles culled
        tri_valid && tri_ready,      // 2 triangles assembled
        fetch_valid && fetch_ready,  // 1 vertices fetched
        busy                         // 0 command cycles
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

    // Vertex Fetch Unit
    vertex_fetch u_vertex_fetch (
        .clk              (clk),
        .rst_n            (rst_n),
        .start            (start_pulse),
        .base_addr        (vbuf_base_addr),
        .vertex_count     (vertex_count),
        .busy             (fetch_busy),
        .error_set        (fetch_err),
        .m_axi_araddr     (m_axi_araddr),
        .m_axi_arlen      (m_axi_arlen),
        .m_axi_arsize     (m_axi_arsize),
        .m_axi_arburst    (m_axi_arburst),
        .m_axi_arvalid    (m_axi_arvalid),
        .m_axi_arready    (m_axi_arready),
        .m_axi_rdata      (m_axi_rdata),
        .m_axi_rresp      (m_axi_rresp),
        .m_axi_rlast      (m_axi_rlast),
        .m_axi_rvalid     (m_axi_rvalid),
        .m_axi_rready     (m_axi_rready),
        .out_x            (fetch_vx),
        .out_y            (fetch_vy),
        .out_z            (fetch_vz),
        .out_color        (fetch_color),
        .out_valid        (fetch_valid),
        .out_ready        (fetch_ready)
    );

    // Geometry Engine
    geom_engine u_geom_engine (
        .clk        (clk),
        .rst_n      (rst_n),
        .mvp        (mvp_matrix),
        .in_x       (fetch_vx),
        .in_y       (fetch_vy),
        .in_z       (fetch_vz),
        .in_color   (fetch_color),
        .in_valid   (fetch_valid),
        .in_ready   (fetch_ready),
        .out_x      (clip_x),
        .out_y      (clip_y),
        .out_z      (clip_z),
        .out_w      (clip_w),
        .out_color  (clip_color),
        .out_valid  (clip_valid),
        .out_ready  (clip_ready),
        .idle       (geom_idle)
    );

    // Perspective Divide & Viewport Scaling
    persp_viewport #(
        .SCREEN_W   (SCREEN_W),
        .SCREEN_H   (SCREEN_H)
    ) u_persp_viewport (
        .clk        (clk),
        .rst_n      (rst_n),
        .in_x       (clip_x),
        .in_y       (clip_y),
        .in_z       (clip_z),
        .in_w       (clip_w),
        .in_color   (clip_color),
        .in_valid   (clip_valid),
        .in_ready   (clip_ready),
        .out_sx     (scr_sx),
        .out_sy     (scr_sy),
        .out_z      (scr_z),
        .out_color  (scr_color),
        .out_flags  (scr_flags),
        .out_valid  (screen_valid),
        .out_ready  (screen_ready),
        .idle       (persp_idle)
    );

    // Interim shim: the baseline assembly/rasterizer still take Q16.16.
    assign screen_x     = {{4{scr_sx[15]}}, scr_sx, 12'b0};
    assign screen_y     = {{4{scr_sy[15]}}, scr_sy, 12'b0};
    assign screen_z     = {16'b0, scr_z};
    assign screen_color = {8'b0, scr_color};

    // Primitive Assembly
    prim_assembly #(
        .DATA_WIDTH (AXI_DATA_WIDTH)
    ) u_prim_assembly (
        .clk         (clk),
        .rst_n       (rst_n),
        .s_v_x       (screen_x),
        .s_v_y       (screen_y),
        .s_v_z       (screen_z),
        .s_v_color   (screen_color),
        .s_v_valid   (screen_valid),
        .s_v_ready   (screen_ready),
        .m_v0_x      (tri_v0_x),
        .m_v0_y      (tri_v0_y),
        .m_v0_z      (tri_v0_z),
        .m_v1_x      (tri_v1_x),
        .m_v1_y      (tri_v1_y),
        .m_v1_z      (tri_v1_z),
        .m_v2_x      (tri_v2_x),
        .m_v2_y      (tri_v2_y),
        .m_v2_z      (tri_v2_z),
        .m_color     (tri_color),
        .m_tri_valid (tri_valid),
        .m_tri_ready (tri_ready)
    );

    // Incremental Pineda Rasterizer
    rasterizer #(
        .DATA_WIDTH (AXI_DATA_WIDTH),
        .FRAC_BITS  (FRAC_BITS),
        .SCREEN_W   (SCREEN_W),
        .SCREEN_H   (SCREEN_H)
    ) u_rasterizer (
        .clk         (clk),
        .rst_n       (rst_n),
        .s_v0_x      (tri_v0_x),
        .s_v0_y      (tri_v0_y),
        .s_v0_z      (tri_v0_z),
        .s_v1_x      (tri_v1_x),
        .s_v1_y      (tri_v1_y),
        .s_v1_z      (tri_v1_z),
        .s_v2_x      (tri_v2_x),
        .s_v2_y      (tri_v2_y),
        .s_v2_z      (tri_v2_z),
        .s_color     (tri_color),
        .s_tri_valid (tri_valid),
        .s_tri_ready (tri_ready),
        .frag_x      (frag_x),
        .frag_y      (frag_y),
        .frag_z      (frag_z),
        .frag_color  (frag_color),
        .frag_valid  (frag_valid),
        .frag_ready  (frag_ready)
    );

    // Pixel Map (Z-Buffer Depth Test & Memory Address Gen)
    pixel_map #(
        .SCREEN_W (SCREEN_W),
        .SCREEN_H (SCREEN_H)
    ) u_pixel_map (
        .clk            (clk),
        .rst_n          (rst_n),
        .s_frag_x       (frag_x),
        .s_frag_y       (frag_y),
        .s_frag_z       (frag_z),
        .s_frag_color   (frag_color),
        .s_frag_valid   (frag_valid),
        .s_frag_ready   (frag_ready),
        .m_zbuf_rd_addr (m_zbuf_rd_addr),
        .m_zbuf_rd_en   (m_zbuf_rd_en),
        .s_zbuf_rd_data (s_zbuf_rd_data),
        .m_zbuf_wr_addr (m_zbuf_wr_addr),
        .m_zbuf_wr_data (m_zbuf_wr_data),
        .m_zbuf_wr_en   (m_zbuf_wr_en),
        .m_fb_wr_addr   (m_fb_wr_addr),
        .m_fb_wr_data   (m_fb_wr_data),
        .m_fb_wr_en     (m_fb_wr_en)
    );

    // Dual-Port BRAM Framebuffer
    framebuffer #(
        .SCREEN_W (SCREEN_W),
        .SCREEN_H (SCREEN_H),
        .DATA_WIDTH (32)
    ) u_framebuffer (
        .clk       (clk),
        .s_wr_addr (m_fb_wr_addr),
        .s_wr_data (m_fb_wr_data),
        .s_wr_en   (m_fb_wr_en),
        .s_rd_addr (32'b0), // Unused until VGA controller is added
        /* verilator lint_off PINCONNECTEMPTY */
        .m_rd_data () // Not connected until VGA controller is added
        /* verilator lint_on PINCONNECTEMPTY */
    );

endmodule

`default_nettype wire
