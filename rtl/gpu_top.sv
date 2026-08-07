`default_nettype none

module gpu_top #(
    parameter integer AXI_ADDR_WIDTH = 32,
    parameter integer AXI_DATA_WIDTH = 32,
    parameter integer FRAC_BITS      = 16,
    parameter integer SCREEN_W       = 640,
    parameter integer SCREEN_H       = 480
)(
    input wire clk,
    input wire rst_n,

    // AXI-Lite Slave Interface
    /* verilator lint_off UNUSEDSIGNAL */
    input wire [AXI_ADDR_WIDTH-1:0] s_axi_awaddr,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire s_axi_awvalid,
    output logic s_axi_awready,

    input wire [AXI_DATA_WIDTH-1:0] s_axi_wdata,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire [3:0] s_axi_wstrb,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire s_axi_wvalid,
    output logic s_axi_wready,

    output logic [1:0] s_axi_bresp,
    output logic s_axi_bvalid,
    input wire s_axi_bready,

    /* verilator lint_off UNUSEDSIGNAL */
    input wire [AXI_ADDR_WIDTH-1:0] s_axi_araddr,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire s_axi_arvalid,
    output logic s_axi_arready,

    output logic [AXI_DATA_WIDTH-1:0] s_axi_rdata,
    output logic [1:0] s_axi_rresp,
    output logic s_axi_rvalid,
    input wire s_axi_rready,

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

    // Transformed Output Stream (Screen Space Pixels)
    output logic signed [31:0] stream_x_screen,
    output logic signed [31:0] stream_y_screen,
    output logic signed [31:0] stream_z_depth,
    output logic [31:0]        stream_color,
    output logic               stream_valid,
    input wire                 stream_ready
);

    // Control registers from AXI-Lite
    logic start_pulse;
    logic [AXI_ADDR_WIDTH-1:0] vbuf_base_addr;
    logic [31:0] vertex_count;
    logic busy;
    logic done_pulse;
    logic signed [31:0] mvp_matrix [0:3][0:3];

    // Fetch -> Geom interconnect
    logic signed [31:0] fetch_vx, fetch_vy, fetch_vz;
    logic [31:0] fetch_color;
    logic fetch_valid, fetch_ready;

    // Geom -> Persp interconnect
    logic signed [31:0] clip_x, clip_y, clip_z, clip_w;
    logic [31:0] clip_color;
    logic clip_valid, clip_ready;

    // Unused AXI-Lite read channels
    assign s_axi_arready = 1'b0;
    assign s_axi_rdata   = '0;
    assign s_axi_rresp   = 2'b00;
    assign s_axi_rvalid  = 1'b0;

    // Register File
    axi_lite_s_intf #(
        .S_AXI_ADDR_WIDTH (7),
        .S_AXI_DATA_WIDTH (AXI_DATA_WIDTH)
    ) u_axi_lite_s_intf (
        .S_AXI_CLK              (clk),
        .S_AXI_RESETN           (rst_n),
        .S_AXI_WRITE_ADDR       (s_axi_awaddr[6:0]),
        .S_AXI_WRITE_ADDR_VALID (s_axi_awvalid),
        .S_AXI_WRITE_ADDR_READY (s_axi_awready),
        .S_AXI_WRITE_DATA       (s_axi_wdata),
        .S_AXI_WRITE_DATA_VALID (s_axi_wvalid),
        .S_AXI_WRITE_DATA_READY (s_axi_wready),
        .S_AXI_BRESP            (s_axi_bresp),
        .S_AXI_BVALID           (s_axi_bvalid),
        .S_AXI_BREADY           (s_axi_bready),
        .start_pulse            (start_pulse),
        .vbuf_base_addr         (vbuf_base_addr),
        .vertex_count           (vertex_count),
        .mvp_matrix             (mvp_matrix)
    );

    // Vertex Fetch Unit
    vertex_fetch #(
        .AXI_ADDR_WIDTH (AXI_ADDR_WIDTH),
        .AXI_DATA_WIDTH (AXI_DATA_WIDTH)
    ) u_vertex_fetch (
        .clk              (clk),
        .rst_n            (rst_n),
        .start_pulse      (start_pulse),
        .vbuf_base_addr   (vbuf_base_addr),
        .vertex_count     (vertex_count),
        .busy             (busy),
        .done_pulse       (done_pulse),
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
        .stream_vx        (fetch_vx),
        .stream_vy        (fetch_vy),
        .stream_vz        (fetch_vz),
        .stream_color     (fetch_color),
        .stream_valid     (fetch_valid),
        .stream_ready     (fetch_ready)
    );

    // Geometry Engine (4x4 Matrix Multiply)
    geom_engine #(
        .DATA_WIDTH (AXI_DATA_WIDTH),
        .FRAC_BITS  (FRAC_BITS)
    ) u_geom_engine (
        .clk              (clk),
        .rst_n            (rst_n),
        .mvp_matrix       (mvp_matrix),
        .s_stream_vx      (fetch_vx),
        .s_stream_vy      (fetch_vy),
        .s_stream_vz      (fetch_vz),
        .s_stream_color   (fetch_color),
        .s_stream_valid   (fetch_valid),
        .s_stream_ready   (fetch_ready),
        .m_stream_x_clip  (clip_x),
        .m_stream_y_clip  (clip_y),
        .m_stream_z_clip  (clip_z),
        .m_stream_w_clip  (clip_w),
        .m_stream_color   (clip_color),
        .m_stream_valid   (clip_valid),
        .m_stream_ready   (clip_ready)
    );

    // Perspective Divide & Viewport Scaling
    persp_viewport #(
        .DATA_WIDTH (AXI_DATA_WIDTH),
        .FRAC_BITS  (FRAC_BITS),
        .SCREEN_W   (SCREEN_W),
        .SCREEN_H   (SCREEN_H)
    ) u_persp_viewport (
        .clk               (clk),
        .rst_n             (rst_n),
        .s_stream_x_clip   (clip_x),
        .s_stream_y_clip   (clip_y),
        .s_stream_z_clip   (clip_z),
        .s_stream_w_clip   (clip_w),
        .s_stream_color    (clip_color),
        .s_stream_valid    (clip_valid),
        .s_stream_ready    (clip_ready),
        .m_stream_x_screen (stream_x_screen),
        .m_stream_y_screen (stream_y_screen),
        .m_stream_z_depth  (stream_z_depth),
        .m_stream_color    (stream_color),
        .m_stream_valid    (stream_valid),
        .m_stream_ready    (stream_ready)
    );

endmodule

`default_nettype wire
