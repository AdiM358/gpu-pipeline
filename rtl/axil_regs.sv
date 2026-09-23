`default_nettype none

// AXI4-Lite slave + GPU register file. Register map: docs/REGMAP.md.
//
// Write channel: AW and W are accepted independently (in either order or in
// the same cycle) into one-entry holding registers; the write is performed
// once both are held and no B response is pending. WSTRB is honoured.
// Read channel: one outstanding read; data is registered on the AR handshake.
// Unmapped addresses return SLVERR (reads return 0).
module axil_regs #(
    parameter int ADDR_W   = 8,
    parameter int FB_W     = 320,
    parameter int FB_H     = 240,
    parameter int N_PERF   = 8
)(
    input  wire                 clk,
    input  wire                 rst_n,

    // AXI4-Lite slave
    input  wire  [ADDR_W-1:0]   s_axi_awaddr,
    input  wire                 s_axi_awvalid,
    output logic                s_axi_awready,
    input  wire  [31:0]         s_axi_wdata,
    input  wire  [3:0]          s_axi_wstrb,
    input  wire                 s_axi_wvalid,
    output logic                s_axi_wready,
    output logic [1:0]          s_axi_bresp,
    output logic                s_axi_bvalid,
    input  wire                 s_axi_bready,
    input  wire  [ADDR_W-1:0]   s_axi_araddr,
    input  wire                 s_axi_arvalid,
    output logic                s_axi_arready,
    output logic [31:0]         s_axi_rdata,
    output logic [1:0]          s_axi_rresp,
    output logic                s_axi_rvalid,
    input  wire                 s_axi_rready,

    // Commands (single-cycle pulses; the controller ignores them while busy)
    output logic                cmd_draw,
    output logic                cmd_clear,

    // Configuration
    output logic [31:0]         vbuf_base,
    output logic [31:0]         vertex_count,
    output logic                cull_back_en,
    output logic                front_cw,
    output logic [15:0]         clear_color,
    output logic signed [31:0]  mvp [4][4],
    output logic                irq,

    // Status and performance counters from the core
    input  wire                 busy,
    input  wire                 done_set,     // pulse: command finished
    input  wire                 err_set,      // pulse: fetch bus error
    input  wire  [31:0]         perf [N_PERF]
);

    localparam logic [1:0] RESP_OKAY   = 2'b00;
    localparam logic [1:0] RESP_SLVERR = 2'b10;
    localparam logic [31:0] GPU_ID     = 32'h4750_5531;  // "GPU1"

    // Word offsets (byte offset / 4)
    localparam int R_CTRL        = 'h00 >> 2;
    localparam int R_STATUS      = 'h04 >> 2;
    localparam int R_IRQ_EN      = 'h08 >> 2;
    localparam int R_ID          = 'h0C >> 2;
    localparam int R_VBUF_BASE   = 'h10 >> 2;
    localparam int R_VERTEX_CNT  = 'h14 >> 2;
    localparam int R_RASTER_CFG  = 'h18 >> 2;
    localparam int R_CLEAR_COLOR = 'h1C >> 2;
    localparam int R_FB_INFO     = 'h20 >> 2;
    localparam int R_PERF0       = 'h40 >> 2;
    localparam int R_MVP0        = 'h80 >> 2;

    localparam int WA = ADDR_W - 2;  // word address width

    // Registers are word-aligned; byte-offset bits [1:0] are ignored.
    /* verilator lint_off UNUSEDSIGNAL */
    wire unused_addr_lsbs = &{1'b0, s_axi_awaddr[1:0], s_axi_araddr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

    // ---------------------------------------------------------------- state
    logic        done_q, err_q;
    logic [1:0]  irq_en_q;
    logic [1:0]  raster_cfg_q;

    assign cull_back_en = raster_cfg_q[0];
    assign front_cw     = raster_cfg_q[1];
    assign irq          = (done_q & irq_en_q[0]) | (err_q & irq_en_q[1]);

    function automatic logic [31:0] apply_strb(input logic [31:0] old, input logic [31:0] data,
                                               input logic [3:0] strb);
        for (int b = 0; b < 4; b++)
            if (strb[b]) old[8*b +: 8] = data[8*b +: 8];
        return old;
    endfunction

    function automatic logic is_mapped(input logic [WA-1:0] w);
        return (int'(w) <= R_FB_INFO) ||
               (int'(w) >= R_PERF0 && int'(w) < R_PERF0 + N_PERF) ||
               (int'(w) >= R_MVP0  && int'(w) < R_MVP0 + 16);
    endfunction

    // ---------------------------------------------------------------- write path
    logic          aw_held, w_held;
    logic [WA-1:0] aw_word;
    logic [31:0]   w_data;
    logic [3:0]    w_strb;

    assign s_axi_awready = !aw_held;
    assign s_axi_wready  = !w_held;

    wire do_write = aw_held && w_held && !s_axi_bvalid;
    wire wr_ctrl  = do_write && aw_word == WA'(R_CTRL) && w_strb[0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aw_held      <= 1'b0;
            w_held       <= 1'b0;
            aw_word      <= '0;
            w_data       <= '0;
            w_strb       <= '0;
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= RESP_OKAY;
            cmd_draw     <= 1'b0;
            cmd_clear    <= 1'b0;
            vbuf_base    <= '0;
            vertex_count <= '0;
            raster_cfg_q <= '0;
            clear_color  <= '0;
            irq_en_q     <= '0;
            done_q       <= 1'b0;
            err_q        <= 1'b0;
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    mvp[r][c] <= '0;
        end else begin
            cmd_draw  <= 1'b0;
            cmd_clear <= 1'b0;

            if (s_axi_awvalid && s_axi_awready) begin
                aw_held <= 1'b1;
                aw_word <= s_axi_awaddr[ADDR_W-1:2];
            end
            if (s_axi_wvalid && s_axi_wready) begin
                w_held <= 1'b1;
                w_data <= s_axi_wdata;
                w_strb <= s_axi_wstrb;
            end
            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;

            if (do_write) begin
                aw_held      <= 1'b0;
                w_held       <= 1'b0;
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= is_mapped(aw_word) ? RESP_OKAY : RESP_SLVERR;

                case (int'(aw_word))
                    R_CTRL: if (w_strb[0]) begin
                        cmd_draw  <= w_data[0];
                        cmd_clear <= w_data[1];
                    end
                    R_STATUS: if (w_strb[0]) begin   // write-1-to-clear
                        if (w_data[1]) done_q <= 1'b0;
                        if (w_data[2]) err_q  <= 1'b0;
                    end
                    R_IRQ_EN:      irq_en_q     <= 2'(apply_strb(32'(irq_en_q), w_data, w_strb));
                    R_VBUF_BASE:   vbuf_base    <= apply_strb(vbuf_base, w_data, w_strb) & 32'hFFFF_FFF0;
                    R_VERTEX_CNT:  vertex_count <= apply_strb(vertex_count, w_data, w_strb);
                    R_RASTER_CFG:  raster_cfg_q <= 2'(apply_strb(32'(raster_cfg_q), w_data, w_strb));
                    R_CLEAR_COLOR: clear_color  <= 16'(apply_strb(32'(clear_color), w_data, w_strb));
                    default: begin
                        if (int'(aw_word) >= R_MVP0 && int'(aw_word) < R_MVP0 + 16) begin
                            automatic int idx = int'(aw_word) - R_MVP0;
                            mvp[idx / 4][idx % 4] <= apply_strb(mvp[idx / 4][idx % 4], w_data, w_strb);
                        end
                    end
                endcase
            end

            // A new command clears the previous DONE; completion events win
            // over a simultaneous write-1-to-clear so they are never lost.
            if (wr_ctrl && (w_data[0] || w_data[1]) && !busy) done_q <= 1'b0;
            if (done_set) done_q <= 1'b1;
            if (err_set)  err_q  <= 1'b1;
        end
    end

    // ---------------------------------------------------------------- read path
    logic [31:0] rd_mux;
    logic        rd_ok;

    wire [WA-1:0] ar_word = s_axi_araddr[ADDR_W-1:2];
    int           wi;
    assign wi = int'(ar_word);

    always_comb begin
        rd_ok  = is_mapped(ar_word);
        rd_mux = '0;
        case (wi)
            R_CTRL:        rd_mux = '0;
            R_STATUS:      rd_mux = {29'd0, err_q, done_q, busy};
            R_IRQ_EN:      rd_mux = {30'd0, irq_en_q};
            R_ID:          rd_mux = GPU_ID;
            R_VBUF_BASE:   rd_mux = vbuf_base;
            R_VERTEX_CNT:  rd_mux = vertex_count;
            R_RASTER_CFG:  rd_mux = {30'd0, raster_cfg_q};
            R_CLEAR_COLOR: rd_mux = {16'd0, clear_color};
            R_FB_INFO:     rd_mux = {16'(FB_H), 16'(FB_W)};
            default: begin
                if (wi >= R_PERF0 && wi < R_PERF0 + N_PERF)
                    rd_mux = perf[wi - R_PERF0];
                else if (wi >= R_MVP0 && wi < R_MVP0 + 16)
                    rd_mux = mvp[(wi - R_MVP0) / 4][(wi - R_MVP0) % 4];
            end
        endcase
    end

    assign s_axi_arready = !s_axi_rvalid;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rdata  <= '0;
            s_axi_rresp  <= RESP_OKAY;
        end else begin
            if (s_axi_arvalid && s_axi_arready) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rdata  <= rd_mux;
                s_axi_rresp  <= rd_ok ? RESP_OKAY : RESP_SLVERR;
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
