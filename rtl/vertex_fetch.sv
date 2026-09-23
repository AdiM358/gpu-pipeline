`default_nettype none

// Vertex fetch: AXI4 burst reader with credit-based prefetch.
//
// Vertex format (16 bytes, little-endian words): x, y, z (Q16.16), color
// (0x00RRGGBB). Every fetched byte is used.
//
// Read requests are issued ahead of consumption: a burst of up to
// BURST_VERTS vertices is issued whenever the vertex FIFO has room for it
// counting data already in flight ("credits"). So the FIFO can never
// overflow and RREADY can stay high, and up to MAX_BURSTS bursts overlap
// memory latency with streaming. Bursts are split so they never cross a
// 4 KB boundary (an AXI rule).
//
// Errors: a non-OKAY RRESP or an RLAST that disagrees with the requested
// burst length raises error_set once, stops new requests, discards buffered
// and incoming vertices, and ends the job once all outstanding bursts have
// drained (so the bus is left clean for the next job).
module vertex_fetch #(
    parameter int BURST_VERTS = 4,    // 16 beats
    parameter int FIFO_VERTS  = 16,
    parameter int MAX_BURSTS  = 4
)(
    input  wire                clk,
    input  wire                rst_n,

    // Job control
    input  wire                start,
    input  wire  [31:0]        base_addr,      // 16-byte aligned
    input  wire  [31:0]        vertex_count,
    output logic               busy,
    output logic               error_set,      // pulse on first error of a job

    // AXI4 read master (AR + R channels)
    output logic [31:0]        m_axi_araddr,
    output logic [7:0]         m_axi_arlen,
    output logic [2:0]         m_axi_arsize,
    output logic [1:0]         m_axi_arburst,
    output logic               m_axi_arvalid,
    input  wire                m_axi_arready,
    input  wire  [31:0]        m_axi_rdata,
    input  wire  [1:0]         m_axi_rresp,
    input  wire                m_axi_rlast,
    input  wire                m_axi_rvalid,
    output logic               m_axi_rready,

    // Vertex stream out
    output logic signed [31:0] out_x,
    output logic signed [31:0] out_y,
    output logic signed [31:0] out_z,
    output logic [31:0]        out_color,
    output logic               out_valid,
    input  wire                out_ready
);

    localparam int CW = $clog2(FIFO_VERTS + 1);   // vertex-count width
    localparam int PW = $clog2(FIFO_VERTS);       // FIFO pointer width
    localparam int BW = $clog2(MAX_BURSTS + 1);
    localparam int LW = $clog2(BURST_VERTS + 1);  // burst length in vertices

    assign m_axi_arsize  = 3'd2;   // 4-byte beats
    assign m_axi_arburst = 2'b01;  // INCR
    assign m_axi_rready  = 1'b1;   // space is reserved before a burst is issued

    // ------------------------------------------------------------ job state
    logic        active, err;
    logic [31:0] req_addr;
    logic [31:0] req_left;         // vertices not yet requested
    logic [CW-1:0] inflight;       // vertices requested, not yet in the FIFO
    logic [BW-1:0] bursts_out;     // bursts requested, not yet complete

    // ------------------------------------------------------------ vertex FIFO
    logic [127:0]  fifo_mem [FIFO_VERTS];
    logic [PW-1:0] wr_ptr, rd_ptr;
    logic [CW-1:0] fifo_count;

    // ------------------------------------------------------------ burst length FIFO
    logic [LW-1:0] len_q [MAX_BURSTS];
    logic [$clog2(MAX_BURSTS)-1:0] len_wr, len_rd;

    // ------------------------------------------------------------ issue logic
    // Vertices left before the next 4 KB boundary (address is 16 B aligned).
    wire [8:0]  to_4k       = 9'd256 - {1'b0, req_addr[11:4]};
    wire [31:0] want        = (req_left < 32'(BURST_VERTS)) ? req_left : 32'(BURST_VERTS);
    wire [LW-1:0] burst_v   = (32'(to_4k) < want) ? LW'(to_4k) : LW'(want);
    wire [CW:0] committed   = (CW+1)'(fifo_count) + (CW+1)'(inflight);
    wire        room        = committed + (CW+1)'(burst_v) <= (CW+1)'(FIFO_VERTS);
    wire        ar_free     = !m_axi_arvalid || m_axi_arready;
    wire        can_issue   = active && !err && req_left != 0 && room &&
                              bursts_out < BW'(MAX_BURSTS) && ar_free;

    // ------------------------------------------------------------ receive logic
    wire         r_hs      = m_axi_rvalid && m_axi_rready;
    logic [1:0]  word_idx;             // word within vertex
    logic [5:0]  beat_idx;             // beat within burst
    logic [95:0] asm_words;            // x, y, z of the vertex being assembled
    wire  [5:0]  last_beat = 6'({len_q[len_rd], 2'b00}) - 6'd1;
    wire         exp_last  = (beat_idx == last_beat);
    wire         bad_beat  = r_hs && ((m_axi_rresp != 2'b00) || (m_axi_rlast != exp_last));
    wire         vtx_done  = r_hs && word_idx == 2'd3;
    wire         push      = vtx_done && !err && !bad_beat;
    wire         burst_end = r_hs && exp_last;

    // ------------------------------------------------------------ output stage
    wire pop = (fifo_count != 0) && (!out_valid || out_ready) && !err;

    wire start_job = start && !active;

    // Vertices are 16-byte aligned; the low address bits are ignored.
    /* verilator lint_off UNUSEDSIGNAL */
    wire unused_base_lsbs = &{1'b0, base_addr[3:0]};
    /* verilator lint_on UNUSEDSIGNAL */

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active        <= 1'b0;
            err           <= 1'b0;
            error_set     <= 1'b0;
            req_addr      <= '0;
            req_left      <= '0;
            inflight      <= '0;
            bursts_out    <= '0;
            m_axi_arvalid <= 1'b0;
            m_axi_araddr  <= '0;
            m_axi_arlen   <= '0;
            wr_ptr        <= '0;
            rd_ptr        <= '0;
            fifo_count    <= '0;
            len_wr        <= '0;
            len_rd        <= '0;
            word_idx      <= '0;
            beat_idx      <= '0;
            out_valid     <= 1'b0;
        end else begin
            error_set <= 1'b0;

            if (start_job) begin
                active   <= (vertex_count != 0);
                err      <= 1'b0;
                req_addr <= {base_addr[31:4], 4'b0};
                req_left <= vertex_count;
                word_idx <= '0;
                beat_idx <= '0;
            end

            // --- AR channel
            if (m_axi_arvalid && m_axi_arready) m_axi_arvalid <= 1'b0;
            if (can_issue) begin
                m_axi_arvalid     <= 1'b1;
                m_axi_araddr      <= req_addr;
                m_axi_arlen       <= 8'({burst_v, 2'b00}) - 8'd1;
                len_wr            <= len_wr + 1'b1;
                req_addr          <= req_addr + 32'({burst_v, 4'b0});
                req_left          <= req_left - 32'(burst_v);
            end

            // --- R channel
            if (r_hs) begin
                word_idx <= word_idx + 1'b1;
                if (burst_end) begin
                    beat_idx <= '0;
                    len_rd   <= len_rd + 1'b1;
                end else begin
                    beat_idx <= beat_idx + 1'b1;
                end
            end
            if (bad_beat && !err) begin
                err       <= 1'b1;
                error_set <= 1'b1;
            end

            // --- occupancy bookkeeping
            inflight   <= inflight + (can_issue ? CW'(burst_v) : '0) - (vtx_done ? CW'(1) : '0);
            bursts_out <= bursts_out + (can_issue ? BW'(1) : '0) - (burst_end ? BW'(1) : '0);

            // --- FIFO (flushed on error)
            if (push) wr_ptr <= wr_ptr + 1'b1;
            if (pop) rd_ptr <= rd_ptr + 1'b1;
            if (err) begin
                rd_ptr     <= wr_ptr;
                fifo_count <= '0;
            end else begin
                fifo_count <= fifo_count + (push ? CW'(1) : '0) - (pop ? CW'(1) : '0);
            end

            // --- output register
            if (out_valid && out_ready) out_valid <= 1'b0;
            if (pop) out_valid <= 1'b1;
            if (err) out_valid <= 1'b0;

            // --- job completion: everything requested has arrived and left
            if (active && !start_job && bursts_out == 0 && !m_axi_arvalid && !can_issue &&
                (err || (req_left == 0 && fifo_count == 0 && !out_valid)))
                active <= 1'b0;
        end
    end

    // Storage and data-path registers: no reset (lets the FIFO map to LUTRAM).
    always_ff @(posedge clk) begin
        if (can_issue) len_q[len_wr] <= burst_v;
        if (r_hs) begin
            case (word_idx)
                2'd0: asm_words[31:0]  <= m_axi_rdata;
                2'd1: asm_words[63:32] <= m_axi_rdata;
                2'd2: asm_words[95:64] <= m_axi_rdata;
                default: ;
            endcase
        end
        if (push) fifo_mem[wr_ptr] <= {m_axi_rdata, asm_words};
        if (pop)  {out_color, out_z, out_y, out_x} <= fifo_mem[rd_ptr];
    end

    assign busy = active;

endmodule

`default_nettype wire
