module vertex_fetch #(
    parameter integer AXI_ADDR_WIDTH = 32,
    parameter integer AXI_DATA_WIDTH = 32
)(
    // Clock & Reset
    input wire clk,
    input wire rst_n,

    // Control Register File Interface (Inputs from AXI-Lite Slave)
    input wire start_pulse,
    input wire [AXI_ADDR_WIDTH-1:0] vbuf_base_addr,
    input wire [31:0] vertex_count,
    output logic busy,
    output logic done_pulse,

    // AXI4 Master Read Address Channel (AR)
    output logic [AXI_ADDR_WIDTH-1:0] m_axi_araddr,
    output logic [7:0] m_axi_arlen,       // Burst length - 1
    output logic [2:0] m_axi_arsize,      // Bytes per transfer (2 = 4 bytes)
    output logic [1:0] m_axi_arburst,     // Burst type (01 = INCR)
    output logic m_axi_arvalid,
    input wire m_axi_arready,

    // AXI4 Master Read Data Channel (R)
    input wire [AXI_DATA_WIDTH-1:0] m_axi_rdata,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire [1:0] m_axi_rresp,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire m_axi_rlast,
    input wire m_axi_rvalid,
    output logic m_axi_rready,

    // Stream Output to Geometry Engine / Matrix Multiplier
    output logic signed [31:0] stream_vx,
    output logic signed [31:0] stream_vy,
    output logic signed [31:0] stream_vz,
    output logic [31:0] stream_color,
    output logic stream_valid,
    input wire stream_ready
);

    // FSM States
    typedef enum logic [2:0] {
        IDLE        = 3'b000,
        FETCH_ADDR  = 3'b001,
        FETCH_DATA  = 3'b010,
        STREAM_OUT  = 3'b011,
        DONE        = 3'b100
    } state_t;

    state_t state;

    // Internal counters and registers
    logic [AXI_ADDR_WIDTH-1:0] current_addr;
    logic [31:0] vertices_remaining;
    logic [2:0] word_counter; // 0 to 7 (8 words per vertex)

    // Temporary storage for vertex attributes being assembled
    logic signed [31:0] v_x, v_y, v_z;
    logic [31:0] v_color;

    // Fixed AXI burst parameters
    assign m_axi_arlen   = 8'd7;   // 8 transfers per burst
    assign m_axi_arsize  = 3'b010; // 4 bytes (32-bit)
    assign m_axi_arburst = 2'b01;  // INCR burst

    assign busy = (state != IDLE);

    // Stream output assignments
    assign stream_vx    = v_x;
    assign stream_vy    = v_y;
    assign stream_vz    = v_z;
    assign stream_color = v_color;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= IDLE;
            m_axi_arvalid      <= 1'b0;
            m_axi_araddr       <= '0;
            m_axi_rready       <= 1'b0;
            stream_valid       <= 1'b0;
            done_pulse         <= 1'b0;
            current_addr       <= '0;
            vertices_remaining <= '0;
            word_counter       <= '0;
            v_x                <= '0;
            v_y                <= '0;
            v_z                <= '0;
            v_color            <= '0;
        end else begin
            done_pulse <= 1'b0;

            case (state)
                IDLE: begin
                    stream_valid <= 1'b0;
                    m_axi_rready <= 1'b0;
                    if (start_pulse && vertex_count > 0) begin
                        current_addr       <= vbuf_base_addr;
                        vertices_remaining <= vertex_count;
                        state              <= FETCH_ADDR;
                    end
                end

                // Issue AXI Read Request
                FETCH_ADDR: begin
                    m_axi_araddr  <= current_addr;
                    m_axi_arvalid <= 1'b1;

                    if (m_axi_arvalid && m_axi_arready) begin
                        m_axi_arvalid <= 1'b0;
                        m_axi_rready  <= 1'b1;
                        word_counter  <= '0;
                        state         <= FETCH_DATA;
                    end
                end

                // Receive 8 words over AXI R channel
                FETCH_DATA: begin
                    if (m_axi_rvalid && m_axi_rready) begin
                        case (word_counter)
                            3'd0: v_x     <= m_axi_rdata;
                            3'd1: v_y     <= m_axi_rdata;
                            3'd2: v_z     <= m_axi_rdata;
                            3'd3: v_color <= m_axi_rdata;
                            default: ; // Remaining words (e.g. u, v, normals)
                        endcase

                        if (m_axi_rlast || word_counter == 3'd7) begin
                            m_axi_rready <= 1'b0;
                            stream_valid <= 1'b1;
                            state        <= STREAM_OUT;
                        end else begin
                            word_counter <= word_counter + 1'b1;
                        end
                    end
                end

                // Handshake stream data to Geometry Engine
                STREAM_OUT: begin
                    if (stream_valid && stream_ready) begin
                        stream_valid       <= 1'b0;
                        vertices_remaining <= vertices_remaining - 1'b1;
                        current_addr       <= current_addr + 32; // Advance 32 bytes

                        if (vertices_remaining == 1) begin
                            done_pulse <= 1'b1;
                            state      <= IDLE;
                        end else begin
                            state      <= FETCH_ADDR;
                        end
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
