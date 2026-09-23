`default_nettype none

// Raster output: depth test and framebuffer write, one fragment per cycle,
// plus the colour (RGB565) and depth (16-bit) buffers and the clear engine.
//
//   R0  accept fragment, compute pixel address y*W + x
//   R1  depth read address presented to the Z RAM (registered: the address
//       fans out to every BRAM of the buffer)
//   R2  RAM access
//   R3  old depth available -> forward -> compare -> write decision
//   W   registered write port; the RAMs commit at the end of this cycle
//
// Read-after-write hazard: a fragment that reads its old depth in the three
// cycles before an earlier fragment's write to the same pixel commits would
// see stale depth. Rather than stalling, R3 compares its address with the
// writes decided in each of the previous three cycles and forwards the
// newest match. The history advances every cycle (not per fragment), so it
// stays correct across bubbles. Clear writes enter the history too.
//
// Equal depth fails the test (strict less-than): the first fragment drawn at
// a given depth wins, matching model::Gpu.
module rop #(
    parameter int SCREEN_W = 320,
    parameter int SCREEN_H = 240,
    parameter int NPIX     = SCREEN_W * SCREEN_H,
    parameter int AW       = $clog2(NPIX)
)(
    input  wire              clk,
    input  wire              rst_n,

    // Fragments from the rasterizer
    input  wire  [9:0]       in_x, in_y,
    input  wire  [15:0]      in_z,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire  [7:0]       in_r, in_g, in_b,   // RGB888, truncated to RGB565 below
    /* verilator lint_on UNUSEDSIGNAL */
    input  wire              in_valid,
    output logic             in_ready,

    // Clear engine
    input  wire              start_clear,
    input  wire  [15:0]      clear_color,
    output logic             clear_busy,

    // External read port (scan-out / readback), 2-cycle latency.
    // Colour reads work at any time; depth reads share the Z RAM read port
    // with the depth test and are only valid while no draw is in progress.
    input  wire  [AW-1:0]    ext_addr,
    input  wire              ext_depth,
    output logic [15:0]      ext_data,

    output logic             frag_pass,     // pulse per fragment written
    output logic             idle
);

    // ---------------------------------------------------------------- clear engine
    logic [AW-1:0] clr_addr;
    assign in_ready = !clear_busy;

    // ---------------------------------------------------------------- pipeline
    wire accept = in_valid && in_ready;

    logic          v1, v2, v3;
    logic [AW-1:0] a1, a2, a3;
    logic [15:0]   z1, z2, z3;
    logic [15:0]   c1, c2, c3;

    // Write port registers (also forwarding history entry h[0]).
    logic          w_en   [3];
    logic [AW-1:0] w_addr [3];
    logic [15:0]   w_z    [3];
    logic [15:0]   w_c;

    // Z RAM: read address is the fragment's (R1) or the external port's.
    logic [15:0]   z_rdata, c_rdata;

    sdp_ram #(.WIDTH(16), .DEPTH(NPIX)) u_zbuf (
        .clk,
        .we(w_en[0]), .waddr(w_addr[0]), .wdata(w_z[0]),
        .raddr(v1 ? a1 : ext_addr), .rdata(z_rdata)
    );

    sdp_ram #(.WIDTH(16), .DEPTH(NPIX)) u_cbuf (
        .clk,
        .we(w_en[0]), .waddr(w_addr[0]), .wdata(w_c),
        .raddr(ext_addr), .rdata(c_rdata)
    );

    logic [1:0] ext_depth_q;
    always_ff @(posedge clk) ext_depth_q <= {ext_depth_q[0], ext_depth};
    assign ext_data = ext_depth_q[1] ? z_rdata : c_rdata;

    // R3: newest matching write in the last three cycles, else RAM data.
    logic [15:0] z_old;
    always_comb begin
        z_old = z_rdata;
        for (int h = 2; h >= 0; h--)
            if (w_en[h] && w_addr[h] == a3) z_old = w_z[h];
    end
    wire pass = v3 && (z3 < z_old);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v1 <= 1'b0;
            v2 <= 1'b0;
            v3 <= 1'b0;
            for (int h = 0; h < 3; h++) w_en[h] <= 1'b0;
            clear_busy <= 1'b0;
            clr_addr   <= '0;
            frag_pass  <= 1'b0;
        end else begin
            v1 <= accept;
            v2 <= v1;
            v3 <= v2;
            frag_pass <= pass;

            // History shifts every cycle; entry 0 drives the RAM write ports.
            w_en[1] <= w_en[0];
            w_en[2] <= w_en[1];
            if (clear_busy) begin
                w_en[0]  <= 1'b1;
                clr_addr <= clr_addr + 1'b1;
                if (clr_addr == AW'(NPIX - 1)) clear_busy <= 1'b0;
            end else begin
                w_en[0] <= pass;
            end
            if (start_clear && !clear_busy) begin
                clear_busy <= 1'b1;
                clr_addr   <= '0;
            end
        end
    end

    always_ff @(posedge clk) begin
        a1 <= AW'(in_y) * AW'(SCREEN_W) + AW'(in_x);
        z1 <= in_z;
        c1 <= {in_r[7:3], in_g[7:2], in_b[7:3]};
        a2 <= a1;  z2 <= z1;  c2 <= c1;
        a3 <= a2;  z3 <= z2;  c3 <= c2;

        w_addr[1] <= w_addr[0];  w_z[1] <= w_z[0];
        w_addr[2] <= w_addr[1];  w_z[2] <= w_z[1];
        if (clear_busy) begin
            w_addr[0] <= clr_addr;
            w_z[0]    <= 16'hFFFF;
            w_c       <= clear_color;
        end else begin
            w_addr[0] <= a3;
            w_z[0]    <= z3;
            w_c       <= c3;
        end
    end

    assign idle = !v1 && !v2 && !v3 && !w_en[0] && !clear_busy;

endmodule

`default_nettype wire
