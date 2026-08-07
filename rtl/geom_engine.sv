`default_nettype none

module geom_engine #(
    parameter integer DATA_WIDTH = 32,
    parameter integer FRAC_BITS  = 16 // Q16.16 fixed-point format
)(
    input wire clk,
    input wire rst_n,

    // Matrix input from register file (4x4 matrix)
    input wire signed [DATA_WIDTH-1:0] mvp_matrix [0:3][0:3],

    // Input vertex stream (3D Model Space)
    input wire signed [DATA_WIDTH-1:0] s_stream_vx,
    input wire signed [DATA_WIDTH-1:0] s_stream_vy,
    input wire signed [DATA_WIDTH-1:0] s_stream_vz,
    input wire [31:0] s_stream_color,
    input wire s_stream_valid,
    output logic s_stream_ready,

    // Output vertex stream (4D Clip Space)
    output logic signed [DATA_WIDTH-1:0] m_stream_x_clip,
    output logic signed [DATA_WIDTH-1:0] m_stream_y_clip,
    output logic signed [DATA_WIDTH-1:0] m_stream_z_clip,
    output logic signed [DATA_WIDTH-1:0] m_stream_w_clip,
    output logic [31:0] m_stream_color,
    output logic m_stream_valid,
    input wire m_stream_ready
);

    // Q16.16 representation of 1.0 (0x00010000)
    localparam signed [DATA_WIDTH-1:0] FIXED_ONE = 32'sd1 <<< FRAC_BITS;

    // Multiplication regs
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [63:0] prod [0:3][0:3];
    /* verilator lint_off UNUSEDSIGNAL */
    logic [31:0] color_stage1;
    logic valid_stage1;

    // Accumulation and Fixed-Point Scaling regs
    logic signed [DATA_WIDTH-1:0] x_clip_reg;
    logic signed [DATA_WIDTH-1:0] y_clip_reg;
    logic signed [DATA_WIDTH-1:0] z_clip_reg;
    logic signed [DATA_WIDTH-1:0] w_clip_reg;
    logic [31:0] color_stage2;
    logic valid_stage2;

    // Pipeline stall control
    logic pipeline_stall;
    assign pipeline_stall = valid_stage2 && !m_stream_ready;
    assign s_stream_ready = !pipeline_stall;

    // Output assignments
    assign m_stream_x_clip = x_clip_reg;
    assign m_stream_y_clip = y_clip_reg;
    assign m_stream_z_clip = z_clip_reg;
    assign m_stream_w_clip = w_clip_reg;
    assign m_stream_color  = color_stage2;
    assign m_stream_valid  = valid_stage2;

    // Vector v = [vx, vy, vz, 1.0]
    logic signed [DATA_WIDTH-1:0] v_in [0:3];
    assign v_in[0] = s_stream_vx;
    assign v_in[1] = s_stream_vy;
    assign v_in[2] = s_stream_vz;
    assign v_in[3] = FIXED_ONE;

    // Pipeline Execution
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_stage1 <= 1'b0;
            valid_stage2 <= 1'b0;
            color_stage1 <= '0;
            color_stage2 <= '0;
            x_clip_reg   <= '0;
            y_clip_reg   <= '0;
            z_clip_reg   <= '0;
            w_clip_reg   <= '0;

            for (int r = 0; r < 4; r++) begin
                for (int c = 0; c < 4; c++) begin
                    prod[r][c] <= '0;
                end
            end
        end else if (!pipeline_stall) begin
            // Multiply 4x4 MVP matrix by vertex vector
            valid_stage1 <= s_stream_valid;
            if (s_stream_valid) begin
                color_stage1 <= s_stream_color;
                for (int r = 0; r < 4; r++) begin
                    for (int c = 0; c < 4; c++) begin
                        prod[r][c] <= $signed(mvp_matrix[r][c]) * $signed(v_in[c]);
                    end
                end
            end

            // Accumulate products and shift Q32.32 back to Q16.16
            valid_stage2 <= valid_stage1;
            if (valid_stage1) begin
                color_stage2 <= color_stage1;
                x_clip_reg   <= DATA_WIDTH'((prod[0][0] + prod[0][1] + prod[0][2] + prod[0][3]) >>> FRAC_BITS);
                y_clip_reg   <= DATA_WIDTH'((prod[1][0] + prod[1][1] + prod[1][2] + prod[1][3]) >>> FRAC_BITS);
                z_clip_reg   <= DATA_WIDTH'((prod[2][0] + prod[2][1] + prod[2][2] + prod[2][3]) >>> FRAC_BITS);
                w_clip_reg   <= DATA_WIDTH'((prod[3][0] + prod[3][1] + prod[3][2] + prod[3][3]) >>> FRAC_BITS);
            end
        end
    end

endmodule

`default_nettype wire

