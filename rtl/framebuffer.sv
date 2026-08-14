`default_nettype none

module framebuffer #(
    parameter integer SCREEN_W = 640,
    parameter integer SCREEN_H = 480,
    parameter integer DATA_WIDTH = 32
)(
    input wire clk,

    // Port A: GPU Write Interface (From pixel_map)
    input wire [31:0]  s_wr_addr,
    input wire [31:0]  s_wr_data,
    input wire         s_wr_en,

    // Port B: Display Read Interface (To VGA Controller / C++ Testbench)
    input wire [31:0]  s_rd_addr,
    output logic [31:0] m_rd_data
);

    // Memory array (Width * Height words)
    // We make it public so the Verilator C++ testbench can easily read it
    /* verilator lint_off MULTIDRIVEN */
    logic [DATA_WIDTH-1:0] ram_array [0:(SCREEN_W * SCREEN_H)-1] /* verilator public */;
    /* verilator lint_on MULTIDRIVEN */

    // Port A: Synchronous Write
    always_ff @(posedge clk) begin
        if (s_wr_en) begin
            if (s_wr_addr < (SCREEN_W * SCREEN_H)) begin
                ram_array[s_wr_addr] <= s_wr_data;
            end
        end
    end

    // Port B: Synchronous Read
    always_ff @(posedge clk) begin
        if (s_rd_addr < (SCREEN_W * SCREEN_H)) begin
            m_rd_data <= ram_array[s_rd_addr];
        end else begin
            m_rd_data <= '0;
        end
    end

endmodule

`default_nettype wire
