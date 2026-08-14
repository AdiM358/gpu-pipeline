`default_nettype none

module pixel_map #(
    parameter integer SCREEN_W   = 640,
    parameter integer SCREEN_H   = 480
)(
    input wire clk,
    input wire rst_n,

    // Fragment Input Stream from rasterizer
    input wire signed [15:0] s_frag_x,
    input wire signed [15:0] s_frag_y,
    input wire signed [31:0] s_frag_z,
    input wire [31:0]        s_frag_color,
    input wire               s_frag_valid,
    output logic             s_frag_ready,

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

    typedef enum logic [1:0] {
        IDLE,
        WAIT_Z_READ,
        TEST_AND_WRITE
    } state_t;

    state_t state;

    // Latched fragment data
    logic signed [15:0] frag_x_reg, frag_y_reg;
    logic signed [31:0] frag_z_reg;
    logic [31:0]        frag_color_reg;

    // Computed flat memory address
    logic [31:0] pixel_addr;

    // Ready to accept a new fragment only when IDLE
    assign s_frag_ready = (state == IDLE);

    // Compute linear memory address: Addr = Y * Width + X
    always_comb begin
        pixel_addr = (32'(frag_y_reg) * 32'(SCREEN_W)) + 32'(frag_x_reg);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state          <= IDLE;
            frag_x_reg     <= '0;
            frag_y_reg     <= '0;
            frag_z_reg     <= '0;
            frag_color_reg <= '0;
            
            m_zbuf_rd_addr <= '0;
            m_zbuf_rd_en   <= 1'b0;
            
            m_zbuf_wr_addr <= '0;
            m_zbuf_wr_data <= '0;
            m_zbuf_wr_en   <= 1'b0;
            
            m_fb_wr_addr   <= '0;
            m_fb_wr_data   <= '0;
            m_fb_wr_en     <= 1'b0;
        end else begin
            // Default strobes to 0
            m_zbuf_wr_en <= 1'b0;
            m_fb_wr_en   <= 1'b0;
            m_zbuf_rd_en <= 1'b0;

            case (state)
                IDLE: begin
                    if (s_frag_valid && s_frag_ready) begin
                        // Discard fragments outside screen bounds
                        if (s_frag_x >= 16'sd0 && s_frag_x < 16'(SCREEN_W) && 
                            s_frag_y >= 16'sd0 && s_frag_y < 16'(SCREEN_H)) begin
                            
                            frag_x_reg     <= s_frag_x;
                            frag_y_reg     <= s_frag_y;
                            frag_z_reg     <= s_frag_z;
                            frag_color_reg <= s_frag_color;
                            
                            // Immediately request Z-buffer read for this coordinate
                            m_zbuf_rd_addr <= (32'(s_frag_y) * 32'(SCREEN_W)) + 32'(s_frag_x);
                            m_zbuf_rd_en   <= 1'b1;
                            
                            state <= WAIT_Z_READ;
                        end
                    end
                end

                WAIT_Z_READ: begin
                    // Wait 1 clock cycle for standard synchronous BRAM read latency
                    state <= TEST_AND_WRITE;
                end

                TEST_AND_WRITE: begin
                    // Depth Test: Closer fragments have a smaller Z value
                    if (frag_z_reg < signed'(s_zbuf_rd_data)) begin
                        // Pass: Write new Z value to Z-buffer
                        m_zbuf_wr_addr <= pixel_addr;
                        m_zbuf_wr_data <= frag_z_reg;
                        m_zbuf_wr_en   <= 1'b1;

                        // Pass: Write color to Framebuffer
                        m_fb_wr_addr   <= pixel_addr;
                        m_fb_wr_data   <= frag_color_reg;
                        m_fb_wr_en     <= 1'b1;
                    end
                    
                    state <= IDLE;
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
