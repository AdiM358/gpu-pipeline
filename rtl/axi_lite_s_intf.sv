`default_nettype none

module axi_lite_s_intf #(
    parameter integer S_AXI_ADDR_WIDTH = 7, // 7 bits for 128-byte address space
    parameter integer S_AXI_DATA_WIDTH = 32
)(
    // Clock and reset
    input wire S_AXI_CLK,
    input wire S_AXI_RESETN,

    // Write address channel
    /* verilator lint_off UNUSEDSIGNAL */
    input wire [S_AXI_ADDR_WIDTH-1:0] S_AXI_WRITE_ADDR,
    /* verilator lint_off UNUSEDSIGNAL */
    input wire S_AXI_WRITE_ADDR_VALID,
    output logic S_AXI_WRITE_ADDR_READY,

    // Write data channel
    input wire [S_AXI_DATA_WIDTH-1:0] S_AXI_WRITE_DATA,
    input wire S_AXI_WRITE_DATA_VALID,
    output logic S_AXI_WRITE_DATA_READY,

    // Write response channel
    output logic [1:0] S_AXI_BRESP,
    output logic S_AXI_BVALID,
    input wire S_AXI_BREADY,

    // Hardware control register outputs
    output logic start_pulse,
    output logic [31:0] vbuf_base_addr,
    output logic [31:0] vertex_count,
    output logic signed [31:0] mvp_matrix [0:3][0:3]
);

    // 20 x 32-bit register array (Regs 0..3 for control, Regs 4..19 for 4x4 matrix)
    logic [S_AXI_DATA_WIDTH-1:0] reg_file [0:19];
    
    // Latched write address
    logic [4:0] write_reg_idx;

    // Fixed OKAY response
    assign S_AXI_BRESP = 2'b00;

    // Address decoding (Word aligned: drop lower 2 bits)
    assign write_reg_idx = S_AXI_WRITE_ADDR[6:2];

    // Control and status register assignments
    assign vbuf_base_addr = reg_file[1];
    assign vertex_count   = reg_file[2];

    // Regs 4 to 19: 4x4 MVP Matrix map
    always_comb begin
        for (int r = 0; r < 4; r++) begin
            for (int c = 0; c < 4; c++) begin
                mvp_matrix[r][c] = reg_file[4 + (r * 4) + c];
            end
        end
    end

    // AXI write logic and register update
    always_ff @(posedge S_AXI_CLK or negedge S_AXI_RESETN) begin
        if (!S_AXI_RESETN) begin
            S_AXI_WRITE_ADDR_READY <= 1'b0;
            S_AXI_WRITE_DATA_READY <= 1'b0;
            S_AXI_BVALID           <= 1'b0;
            start_pulse            <= 1'b0;

            for (int i = 0; i < 20; i++) begin
                reg_file[i] <= '0;
            end
        end else begin
            // Clear auto-clearing start pulse
            start_pulse <= 1'b0;

            // Generate READY signals when both VALID inputs arrive
            if (!S_AXI_WRITE_ADDR_READY && S_AXI_WRITE_ADDR_VALID && S_AXI_WRITE_DATA_VALID) begin
                S_AXI_WRITE_ADDR_READY <= 1'b1;
                S_AXI_WRITE_DATA_READY <= 1'b1;
            end else begin
                S_AXI_WRITE_ADDR_READY <= 1'b0;
                S_AXI_WRITE_DATA_READY <= 1'b0;
            end

            // Write payload on handshake cycle AND assert BVALID
            if (S_AXI_WRITE_ADDR_VALID && S_AXI_WRITE_DATA_VALID && !S_AXI_BVALID) begin
                if (write_reg_idx < 20) begin
                    reg_file[write_reg_idx] <= S_AXI_WRITE_DATA;
                end
                S_AXI_BVALID <= 1'b1;

                // Trigger self-clearing start pulse on reg 0 write if bit 0 is high
                if (write_reg_idx == 5'h0 && S_AXI_WRITE_DATA[0]) begin
                    start_pulse <= 1'b1;
                end
            end

            // Clear BVALID once host acknowledges with BREADY
            if (S_AXI_BREADY && S_AXI_BVALID) begin
                S_AXI_BVALID <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
