`default_nettype none

module persp_viewport #(
    parameter integer DATA_WIDTH  = 32,
    parameter integer FRAC_BITS   = 16,  // Q16.16 format
    parameter integer SCREEN_W    = 640,
    parameter integer SCREEN_H    = 480
)(
    input wire clk,
    input wire rst_n,

    // Input from geom_engine
    input wire signed [DATA_WIDTH-1:0] s_stream_x_clip,
    input wire signed [DATA_WIDTH-1:0] s_stream_y_clip,
    input wire signed [DATA_WIDTH-1:0] s_stream_z_clip,
    input wire signed [DATA_WIDTH-1:0] s_stream_w_clip,
    input wire [31:0]                  s_stream_color,
    input wire                         s_stream_valid,
    output logic                       s_stream_ready,

    // Output screen space coordinates
    output logic signed [DATA_WIDTH-1:0] m_stream_x_screen,
    output logic signed [DATA_WIDTH-1:0] m_stream_y_screen,
    output logic signed [DATA_WIDTH-1:0] m_stream_z_depth,
    output logic [31:0]                  m_stream_color,
    output logic                         m_stream_valid,
    input wire                           m_stream_ready
);

    // Screen center constants in fixed point
    localparam signed [DATA_WIDTH-1:0] HALF_W = (SCREEN_W / 2) <<< FRAC_BITS;
    localparam signed [DATA_WIDTH-1:0] HALF_H = (SCREEN_H / 2) <<< FRAC_BITS;

    // Pipeline registers for divide
    logic signed [DATA_WIDTH-1:0] x_ndc, y_ndc, z_ndc;
    logic [31:0] color_pipe1;
    logic valid_pipe1;

    // Pipeline registers for viewport math
    logic signed [DATA_WIDTH-1:0] x_screen_reg, y_screen_reg, z_depth_reg;
    logic [31:0] color_pipe2;
    logic valid_pipe2;

    // Pipeline flow control
    logic pipeline_stall;
    assign pipeline_stall = valid_pipe2 && !m_stream_ready;
    assign s_stream_ready = !pipeline_stall;

    assign m_stream_x_screen = x_screen_reg;
    assign m_stream_y_screen = y_screen_reg;
    assign m_stream_z_depth  = z_depth_reg;
    assign m_stream_color    = color_pipe2;
    assign m_stream_valid    = valid_pipe2;

    // Divide clip coords by W to get NDC
    always_comb begin
        if (s_stream_w_clip != 0) begin
            x_ndc = DATA_WIDTH'((64'(s_stream_x_clip) <<< FRAC_BITS) / 64'(s_stream_w_clip));
            y_ndc = DATA_WIDTH'((64'(s_stream_y_clip) <<< FRAC_BITS) / 64'(s_stream_w_clip));
            z_ndc = DATA_WIDTH'((64'(s_stream_z_clip) <<< FRAC_BITS) / 64'(s_stream_w_clip));
        end else begin
            x_ndc = '0;
            y_ndc = '0;
            z_ndc = '0;
        end
    end

    // Intermediate 64-bit viewport products
    logic signed [63:0] x_screen_full, y_screen_full;
    always_comb begin
        x_screen_full = (64'(x_ndc) * 64'(HALF_W)) >>> FRAC_BITS;
        y_screen_full = (64'(y_ndc) * 64'(HALF_H)) >>> FRAC_BITS;
    end

    // Process pipeline steps
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_pipe1  <= 1'b0;
            valid_pipe2  <= 1'b0;
            color_pipe1  <= '0;
            color_pipe2  <= '0;
            x_screen_reg <= '0;
            y_screen_reg <= '0;
            z_depth_reg  <= '0;
        end else if (!pipeline_stall) begin
            // Register divide results
            valid_pipe1 <= s_stream_valid;
            if (s_stream_valid) begin
                color_pipe1 <= s_stream_color;
            end

            // Map NDC (-1 to 1) to screen pixels and invert Y
            valid_pipe2 <= valid_pipe1;
            if (valid_pipe1) begin
                color_pipe2  <= color_pipe1;
                x_screen_reg <= DATA_WIDTH'(x_screen_full + 64'(HALF_W));
                y_screen_reg <= DATA_WIDTH'(64'(HALF_H) - y_screen_full);
                z_depth_reg  <= z_ndc;
            end
        end
    end

endmodule

`default_nettype wire
